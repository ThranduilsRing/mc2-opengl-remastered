# Walls to Static Shadow Pass — Implementation Plan (v4)

## Prerequisite (blocker before any coding)

**Confirm a non-zero, stable, per-instance identifier for the building's full shadow lifetime.** Owner identity is now foundational to cache correctness, idempotent registration, multi-primitive unregister, and double-unregister being a no-op. If this isn't nailed down, everything downstream rots.

Candidates in order of preference:
1. `Building::getWatchID()` — existing watch-ID system, documented not to be recycled within a mission.
2. `Building::getStaticShadowOwnerId()` — GameObject handle. Confirm it's not recycled after object destruction within the same mission (some object-pool implementations do recycle).
3. `reinterpret_cast<uintptr_t>(this)` on the `Building*` — unique while object lives; *never* reuse after delete without confirming the address won't be handed to a different Building.

Requirements for whichever is chosen:
- Non-zero for any live wall.
- Stable for the full duration from `Building::init()` through destruction + unregister.
- Not reused by another live wall during that window.

**Do not proceed to implementation until this is verified in code.** A 10-minute grep + short test that logs IDs across a mission load → destroy → reload cycle is the right way to settle this. All uses of `ownerId` in the plan assume this has been resolved.

## Goal

Move wall buildings (`BUILDING_SUBTYPE_WALL`) from per-frame dynamic shadow collection into the world-fixed static shadow accumulator. Turret/radar buildings and all other dynamic objects (mechs, vehicles, etc.) stay in the dynamic pass unchanged.

## Why

- Per-frame GPU + CPU savings: dozens to hundreds of wall shapes removed from dynamic shadow draw calls every frame.
- Shadow quality: long sunset wall shadows stop clipping at the ~1200-unit dynamic-shadow radius; walls inherit the world-fixed 4096² projection and can cast from off-screen walls whose shadows reach on-screen pixels.
- First proof of the static-prop pattern before we broaden to hangars/depots.

## Guiding principle — from prior session advisor

> "Do not touch update/cull ownership more than necessary. Keep the gameplay/update path exactly as-is and change only which shadow collection bucket a building goes into. Minimize chance of reawakening the cull-gate / resource-pool mess."

See: `memory/cull_gates_are_load_bearing.md`, `memory/tgl_pool_exhaustion_is_silent.md`, `docs/gpu-static-prop-cull-lessons.md`.

## Design pivot from v1 — why walls use a persistent cache, not a per-frame reroute

v1 of this plan rerouted wall shadow submissions via a `SHADOW_COLLECT_{DYNAMIC,STATIC,SUPPRESS}` flag flipped around `bldgShape->Render()` in `BldgAppearance::render()`. That had two fatal holes:

1. **Cull-frustum eats off-screen walls.** `BldgAppearance::render()` only runs for walls currently in view. Static shadow covers the whole 4096² world — a wall off-camera whose long shadow reaches on-screen pixels *must* submit. Per-frame reroute through `render()` can't see those walls.
2. **Rebuild-gate mismatch.** The static terrain rebuild at `txmmgr.cpp:1191` fires on `shadowRebuildForced || camMovedFar > 100²`. On a cam-move rebuild, `gos_ShadowRebuildPending()` is false, so a `needStaticRebuild` check based on it would fail to resubmit walls on exactly the frame we need them.

Walls have a property that makes them much easier than v1 assumed: **they're immobile and populated at mission load.** So we capture each wall's shadow payload (`{vb, ib, vdecl, worldMatrix}`) *once* at `Building::init()` time into a persistent cache, and replay that cache whenever the static pass rebuilds. Zero per-frame cost, no view-cull coupling, no mode-flag plumbing into the render path.

The `SHADOW_COLLECT_*` mode infrastructure is *not* added in this plan. Defer it until we broaden to objects that legitimately need per-frame routing (hangars whose matrices could change, or future dynamic-candidate props).

## Core contracts — ownership, lifetime, idempotency

These are *the* correctness rules of this feature — violating any of them produces the bizarre regressions we're trying to avoid.

1. **Owner identity is a set, not a slot.** One owner (`Building*` handle) may register N primitives. Unregister-by-owner MUST remove *all* entries with that owner. The cache is keyed by ownership, not by index.
2. **Registration is idempotent.** `registerStaticShadow(ownerId)` checks whether `ownerId` already has entries and either (a) returns early, or (b) unregisters them first and re-adds. Chosen: **unregister-then-add** so a mid-mission shape swap (damage LOD, etc.) is correctly reflected. Either way, double-register never creates duplicates.
3. **Dead slots are recycled.** A single free-list head in the cache. Adding a primitive inside an open transaction first consumes from the free list; `g_numWallShadows` only grows when the free list is empty. Destroy/spawn churn over a long mission cannot exhaust `MAX_WALL_SHADOW_ENTRIES` unless the live-set is actually that large.
4. **Handle lifetime: cache does NOT own, and cache does NOT outlive its owner.** `vb/ib/vdecl` are borrowed references to resources owned by `BldgAppearance`/its `TG_Shape`. Every teardown path that frees those resources MUST unregister first. Audit below enumerates them.
5. **Suppression is scoped to a known-narrow callsite, not a process latch.** The global push/pop counter from v2 is removed. Instead, `BldgAppearance::render()` passes an explicit "skip shadow submission" argument into the one `TG_Shape` render path that would otherwise call `addShadowShape()` at `tgl.cpp:2709`. The flag flows as data, not as ambient state. Reentrancy and nested renders are no longer a concern.

## Infrastructure already in place

- `gos_RequestFullShadowRebuild()` / `gos_ShadowRebuildPending()` / `gos_ClearShadowRebuildPending()` — declared in `gos_postprocess.h`, implemented in `gos_postprocess.cpp`.
- Static terrain shadow pass in `mclib/txmmgr.cpp:1189-1239` renders terrain into `shadowFBO_`, gated by `shadowRebuildForced || camMovedFar`, supports multi-frame accumulation, clears only on first frame.
- `addShadowShape()` at `mclib/txmmgr.cpp:97` is the single collection call site used by `tgl.cpp:2709` (used only for the dynamic pass — unchanged by this plan).
- `BuildingType::subType == BUILDING_SUBTYPE_WALL` is set at load in `code/bldng.cpp:308`. Subtype lives on the *type*, shared across instances — query via `getObjectType()->getSubType()`.
- `Building` creates `appearance = new BldgAppearance` at `code/bldng.cpp:1241`, then calls `appearance->init(...)` at `:1248`. The wall-cache registration hook goes *after* that init, once the appearance has shape data.
- Destruction sentinel: `code/bldng.cpp:827` — `(baseTileId != 177) && (status == OBJECT_STATUS_DESTROYED)` fires exactly once per destruction for any building (bridges, walls, generic). Confirmed the block runs for walls (no subtype gate).

## Steps

### 1. Persistent wall shadow cache (new file or in txmmgr.cpp)

Add a world-fixed cache of wall shadow payloads with a free list for slot recycling:

```cpp
struct WallShadowEntry {
    HGOSBUFFER            vb;
    HGOSBUFFER            ib;
    HGOSVERTEXDECLARATION vdecl;
    float                 worldMatrix[16];
    uint32_t              ownerId;      // Building handle; 0 means slot is free
    int                   nextFree;     // free-list link when ownerId == 0; -1 otherwise
};

static const int MAX_WALL_SHADOW_ENTRIES = 2048;  // sized for wolfman + broadening
static WallShadowEntry g_wallShadows[MAX_WALL_SHADOW_ENTRIES];
static int             g_numWallShadows = 0;   // high-water mark (NOT live count)
static int             g_wallShadowFreeHead = -1;  // -1 = no free slots, append

// Public API (gos_static_props.h; included by bdactor.cpp + txmmgr.cpp).
// Owner-level transactions only — no per-primitive append exposed publicly.

// Begin an owner transaction. Asserts ownerId != 0. Any existing entries for
// ownerId are unregistered immediately (idempotent re-registration).
void gos_BeginRegisterWallOwner(uint32_t ownerId);

// Add one primitive to the currently-open owner transaction. Must be called
// between Begin/End. Asserts a transaction is open.
void gos_AddWallOwnerPrimitive(HGOSBUFFER vb, HGOSBUFFER ib,
                               HGOSVERTEXDECLARATION vdecl,
                               const float worldEntries16[16]);

// Close the owner transaction. Calls gos_RequestFullShadowRebuild() so the
// owner's primitives participate in the next accumulation regardless of
// timing vs the first-priming frame.
void gos_EndRegisterWallOwner(void);

// Remove ALL entries for this owner. Slots returned to the free list.
// Double-unregister is a no-op.
void gos_UnregisterWallShadow(uint32_t ownerId);

// Wholesale reset — mission unload.
void gos_ClearAllWallShadows(void);

// Diagnostics.
int  gos_GetWallShadowLiveCount(void);        // excludes dead slots — pass gate
int  gos_GetWallShadowHighWater(void);        // == g_numWallShadows (debug only)
int  gos_GetWallShadowFreeCount(void);
```

**Why transaction API instead of direct per-primitive register:** a bare per-primitive append could be called in isolation — someone deletes all primitives and then re-adds only one, producing a half-registered owner with no error. Begin/Add/End enforces the "all-or-nothing, always-idempotent" contract at the API boundary. Direct per-primitive append is not exposed publicly.

**Transaction threading + nesting:** registration transactions are single-threaded and non-nestable by contract. The `Begin` assertion fires if a transaction is already open; `Add`/`End` assert one *is* open. The engine is single-threaded for gameplay/rendering today, so this is a correctness guard against accidental reentrancy (e.g., a future callback firing mid-registration), not a concurrency primitive.

**Zero-primitive transactions are legal.** A wall that walks its `bldgShape` and finds no renderable primitives (malformed shape, filtered LOD, bug in the walker) may legitimately `Begin` → `End` without any `Add` calls. Result: owner has no cached entries, the prior registration (if any) was still unregistered by `Begin`, and `End` still requests a rebuild. Log WARN in debug builds from `End` if zero primitives were added — it's probably a bug in the walker, but not a contract violation.

Sizing: 2048 × 96 B ≈ 200 KB. Free-list recycling means exhaustion is bounded by the *live* wall count, not lifetime destroy/spawn volume.

**Debug assertions.** `gos_BeginRegisterWallOwner` asserts ownerId != 0 and that no transaction is already open. `gos_AddWallOwnerPrimitive` asserts a transaction is open. `gos_EndRegisterWallOwner` asserts a transaction is open and closes it. A WARN-log on the unregister-inside-Begin path helps catch mysterious LOD/damage-state swaps in the wild.

**Owner ID source** is fixed by the Prerequisite section at the top of this document. Do not revisit here.

### 2. Static wall pass — render the cache (txmmgr.cpp ~line 1234)

Inside the existing `if (shadowRebuildForced || shadowCamDist > ...)` block, **between** `gos_EndShadowPrePass()` (line 1234) and the `if (shadowRebuildForced) gos_ClearShadowRebuildPending();` (line 1236):

```cpp
gos_EndShadowPrePass();

// Static object shadow pass — immobile props (walls today, hangars later)
// accumulated into the same shadow FBO as terrain, using the static light matrix.
// Runs on every static rebuild frame (forced OR camera-move triggered), so both
// rebuild paths pick up the same wall data.
//
// Gate on LIVE count (not high-water g_numWallShadows). With free-list recycling
// the high-water can stay positive after all walls are destroyed — opening the
// pass for zero live entries is needless work and contradicts the live-count
// API contract.
if (gos_GetWallShadowLiveCount() > 0) {
    ZoneScopedN("Shadow.StaticProps");
    TracyGpuZone("Shadow.StaticProps");
    gos_BeginStaticShadowObjectPass();  // binds shadowFBO_, NO clear, static light matrix
    for (int wi = 0; wi < g_numWallShadows; wi++) {
        const WallShadowEntry& e = g_wallShadows[wi];
        if (e.ownerId == 0) continue;  // free slot — ownerId==0 is the canonical "dead" marker
        gos_DrawShadowObjectBatch(e.vb, e.ib, e.vdecl, e.worldMatrix);
    }
    gos_EndStaticShadowObjectPass();
}

if (shadowRebuildForced) gos_ClearShadowRebuildPending();
```

New gos_postprocess functions: `gos_BeginStaticShadowObjectPass` / `gos_EndStaticShadowObjectPass` mirror the dynamic equivalents but bind `shadowFBO_` and set the *static* light-space matrix as the uniform. No clear.

**Critical placement:** this block is *inside* the `if (rebuild)` scope so walls only draw when terrain is also drawing. That keeps the accumulator coherent — we never add wall depth to a static map on a frame where terrain isn't re-accumulating.

**State parity with terrain static pass.** The static wall draw MUST match the static terrain pre-pass for:
- Polygon offset / depth slope-scale bias (same values — mismatched bias between terrain depth and occluder depth is the classic peter-panning / acne source)
- Cull mode (terrain pre-pass face winding vs wall winding — walls are opaque closed shells, terrain is single-sided; confirm the shadow shader doesn't early-out on back-faces differently for the two)
- **Depth test function** (`GL_LESS` vs `GL_LEQUAL` across terrain pre-pass and object pass — a mismatch hides wall depth behind equal-Z terrain pixels on some maps)
- **Depth mask** (`glDepthMask(GL_TRUE)` — same note as `applyRenderStates` workaround elsewhere; if the object pass goes through `gosRenderMaterial::apply()` it may silently clobber depth writes)
- **Color mask** (shadow pass writes depth only; confirm color mask is `GL_FALSE` for both passes — writing to a stale color attachment wastes bandwidth and can flag driver warnings)
- Light-space matrix (static, built once per session via `gos_BuildStaticLightMatrix` — same matrix that drove terrain this frame)
- Viewport + FBO attachments (same `shadowFBO_`, no clear)
- Shadow shader defines (the `shadow_object` program already exists for dynamic objects; reuse unchanged — the matrix binding is what differentiates static vs dynamic)

Action item at implementation: before writing `gos_BeginStaticShadowObjectPass`, diff the GL state between `gos_BeginShadowPrePass()` (terrain) and `gos_BeginDynamicShadowPass()` (current object shadow) and pick the correct per-axis value for each state. Don't blindly "mirror the dynamic pass."

**Single-frame draw-call budget.** On the prime frame (first frame with terrain), *all* registered walls submit in one pass (no multi-frame spread like terrain's camera-reveal accumulation). With 2048-entry cap, worst case is ~2048 draw calls in that pass. That's fine for modern drivers but worth a Tracy GPU zone so a regression is visible.

### 3. Register each wall at load (code/bldng.cpp)

In `Building::init()`, after `appearance->init(...)` returns at line 1248, if we are a wall: extract the shape payload and register.

```cpp
// After appearance->init(...) at bldng.cpp:1248.
// getStaticShadowOwnerId() is a thin helper that returns the verified owner-ID
// source from the Prerequisite section (getWatchID / getHandle / uintptr_t(this)).
// The helper is the single place the ID source is committed — all other call
// sites use it.
if (appearance &&
    getObjectType()->getSubType() == BUILDING_SUBTYPE_WALL) {
    ((BldgAppearance*)appearance)->registerStaticShadow(getStaticShadowOwnerId());
}
```

`BldgAppearance::registerStaticShadow(uint32_t ownerId)` (new method, bdactor.cpp):

```cpp
void BldgAppearance::registerStaticShadow(uint32_t ownerId) {
    gos_BeginRegisterWallOwner(ownerId);   // unregisters any existing entries
    for (each primitive in bldgShape and sub-shapes) {
        gos_AddWallOwnerPrimitive(prim.vb, prim.ib, prim.vdecl, worldToShape);
    }
    gos_EndRegisterWallOwner();            // closes txn, requests rebuild
    m_isWallStaticShadow = true;
}
```

Rebuild request is intentionally unconditional and inside `EndRegisterWallOwner` — at mission load many walls call this in succession, and the `gos_ShadowRebuildPending()` flag is a sticky bool so repeated calls coalesce naturally. Document in the API header: *"Repeated rebuild requests during load are expected and coalesce via the sticky pending flag — no per-call cost."*

**Appearance existence:** `appearance` is guaranteed non-null here — construction at `bldng.cpp:1241` is followed by `gosASSERT(appearance != NULL)` at `:1242` and `init()` at `:1248`. Subtype is on the type (set once in `BuildingType::init` at `:308`), already in place by the time any instance is constructed.

**Timing vs first rebuild — the initialization contract.**

Static shadow priming does NOT occur during the loading screen. It occurs on the *first gameplay frame* once terrain shadow data is available (`txmmgr.cpp:1162-1174`, gated by `s_terrainShadowPrimed` + the existence of real `masterVertexNodes[]` entries). Therefore:

> **Contract:** Wall shadow registration must complete during object init, *before* that first priming/rebuild opportunity. Otherwise the wall misses the initial static accumulation and will only appear when some unrelated event (camera move >100 units, building destruction elsewhere, manual rebuild request) next forces a full rebuild.

Two consequences:

1. **Nominal case (wall exists at mission load).** `Building::init()` runs during load → `registerStaticShadow()` fires → `gos_RequestFullShadowRebuild()` is set (sticky). First gameplay frame: `s_terrainShadowPrimed` fires a rebuild anyway, wall is in the first accumulation. Works with or without the explicit request, but the request is cheap insurance.

2. **Late-spawn case (wall created after the first priming frame).** Mission script, reinforcement spawn, or any future dynamic-placement feature. `registerStaticShadow()` MUST call `gos_RequestFullShadowRebuild()` unconditionally — step 3.4 above. Without it, the wall is silently absent from the static shadow map until an unrelated camera move or destruction event happens to trigger a rebuild, producing the exact "wall visible but casts no shadow" ghost bug that's hardest to reproduce.

Mission load is NOT the real synchronization point — the first eligible shadow frame is. Treat `registerStaticShadow()` as "I need to be in the next accumulation, whenever that happens" rather than "I'll be ready by load-complete."

**LOD / damage / shape swap.** If a wall's `bldgShape` is ever replaced at runtime (damage state mesh swap, LOD transition), the owner of that swap MUST call `registerStaticShadow(getStaticShadowOwnerId())` again. The idempotent unregister-then-add contract means this "just works" as long as the swap site invokes it. Step 3a below audits swap sites.

### 3a. Audit: appearance swap sites

**Hard contract (add to every touched site):** *For any wall owner currently in the cache, `gos_UnregisterWallShadow(ownerId)` MUST be called BEFORE any mutation or free of that owner's shape resources (`TG_Shape`, `vb`, `ib`, `vdecl`).* Not at the "after" site, not in a deferred cleanup — before. This is a hard ordering invariant, not a cleanup hint. Call the new register transaction afterwards with the new resources.

Teardown order reliability is the exact thing v3 review flagged as a race: if `TG_Shape` destruction can run before `Building::~Building`, the cache holds dangling handles until some later unregister catches up. By contract-binding unregister to *any* resource mutation or free (not just destruction), we close that window structurally.

Search `BldgAppearance` for any path that assigns a new `bldgShape` or mutates its VB/IB handles after init. Likely candidates:

- Damage-state LOD swap (`*_dmg.ase` / `*_damaged.ase` variants)
- Distance LOD (if `BldgAppearance` swaps between `HIGH/MED/LOW` shapes)
- Destruction transition (already covered by step 5's unregister)

For each swap site, add `registerStaticShadow(getOwnerId())` after the swap *only if the building is a wall*. If the audit finds no swap sites (plausible — walls are simple), document that in a comment at `registerStaticShadow` so a future contributor who adds a swap knows to re-register.

**Handle lifetime audit.** The `vb/ib/vdecl` cached in the entry are owned by the `TG_Shape` that lives inside the `BldgAppearance`. Teardown paths:

- `Building::destroy` / `~Building` → ~`BldgAppearance` → ~`TG_Shape` → buffers freed. This is the critical path — see step 5.
- Mission unload — all buildings destroyed, all appearances destroyed. Step 6 covers this with `gos_ClearAllWallShadows`.
- GL context loss / reset — MC2 does not currently recover; process exits. Not a concern today but note it for the memory file.

If any other path frees a wall's buffers (e.g., appearance teardown without Building destruction), it must call `gos_UnregisterWallShadow(ownerId)` first. Action item: grep for `delete appearance` and `appearance = NULL` to confirm no such path exists outside Building destruction.

### 4. Suppress walls from the dynamic shadow pass (data flow, not ambient state)

v2 proposed a global push/pop suppression counter around `bldgShape->Render()`. Reviewer correctly flagged this as risky: if `Render()` is reentrant, invokes effects, or nests child objects, the latch can silently swallow unrelated shadow submissions. We replace it with an explicit data-flow flag.

**Path:** `tgl.cpp:2709` is the *only* call site of `addShadowShape()` per current grep. Confirm the enclosing function signature before threading the parameter — it's somewhere in the `TG_Shape::Render` family, but the exact entry point should be identified in code, not assumed here. Add a parameter to the render entry point that threads "skip shadow submission" as a function argument:

```cpp
// TG_Shape render entry point (tgl.h / tgl.cpp)
void TG_Shape::Render(..., bool skipShadowSubmit = false);

// Inside, at tgl.cpp:2709:
if (!skipShadowSubmit) {
    addShadowShape(theShape->vb_, theShape->ib_, theShape->vdecl_, shapeToWorld->entries);
}
```

Caller in `BldgAppearance::render()`:

```cpp
bldgShape->Render(..., /*skipShadowSubmit=*/ m_isWallStaticShadow);
```

**Why this is safer than the v2 latch:**
- Scope is the one function activation that receives the argument. No ambient state to forget to pop on early return, exception, or goto.
- Reentrancy-safe by construction *IF* recursive helper calls don't propagate the flag.

**Recursion audit (prerequisite, not optional).** Before relying on this design, grep `TG_Shape::Render` and any helpers it calls for internal recursive shape renders. For each recursive call, confirm that `skipShadowSubmit` is either:
- explicitly passed `false` (nested child shapes decide for themselves), OR
- explicitly passed through because the nested shape is known to be a sub-primitive of the same wall owner and should also skip.

**Do NOT** let the parameter default-propagate downward. A helper signature like `renderSubShape(...)` that internally calls `Render(..., skipShadowSubmit)` with the outer argument reused would unintentionally suppress shadows for unrelated geometry attached to the wall (damage effects, banners, lights). This is the exact class of bug v3 reviewer was worried about — the data-flow design is safer than the ambient latch *only* if we don't reintroduce pseudo-ambience by threading the flag through recursion carelessly.

Outcome of the audit must be documented in a comment at the `skipShadowSubmit` parameter declaration: "Not propagated to recursive subrenders; each activation decides explicitly."

**Other walls-only shadow paths?** Before implementation, grep for every caller that might submit wall geometry to `addShadowShape` (direct or via `TG_Shape::Render`). If any wall path doesn't route through `BldgAppearance::render()`, it needs the same explicit flag. Action item, not a blocker.

If the parameter-threading turns out to touch too much of `tgl.cpp`, a narrowly-scoped thread-local with a single push/pop exactly around the one known call site is an acceptable fallback — but only once we've confirmed the call is non-reentrant (which the review correctly flagged as an unproven assumption).

### 5. Destruction invalidation hook (bldng.cpp:827)

At the once-per-destruction sentinel:

```cpp
if ((baseTileId != 177) && (status == OBJECT_STATUS_DESTROYED))
{
    baseTileId = 177;  // existing sentinel

    if (getObjectType()->getSubType() == BUILDING_SUBTYPE_WALL) {
        gos_UnregisterWallShadow(getStaticShadowOwnerId());  // returns ALL slots for this owner to free list
        gos_RequestFullShadowRebuild();         // re-accumulate without this wall
    }

    // ... rest of existing block ...
}
```

`gos_UnregisterWallShadow(ownerId)` scans for *every* entry matching `ownerId` (multi-primitive walls produce multiple entries — all must go), zeros each entry's `ownerId`, and pushes each slot onto the free list. The next registration transaction reuses free-list slots via `gos_AddWallOwnerPrimitive` before growing the high-water mark.

**Critical: also unregister on non-destruction teardown paths.** If `Building::destroy()` (or the destructor) runs without `OBJECT_STATUS_DESTROYED` ever being set — e.g., mission abort, object removed by a script — `gos_UnregisterWallShadow` must still fire. Safest site: `Building::~Building` or whatever the universal teardown hook is. Otherwise the cache holds dangling `vb/ib/vdecl` references after mission abort.

Implementation note: add unregister to *both* the destruction sentinel (for correct shadow update during gameplay destruction) and the universal destructor (for correctness on abnormal teardown). Double-unregister is a no-op by design.

### 6. Mission lifecycle

- **Mission load:** walls register themselves via step 3 as they're constructed. `registerStaticShadow` sets `gos_RequestFullShadowRebuild()` (sticky). The first gameplay frame's `s_terrainShadowPrimed` path would rebuild anyway, so belt + suspenders.
- **Mission unload:** call `gos_ClearAllWallShadows()` to reset the cache + free list to pristine. Place this alongside the existing mission-unload teardown that destroys buildings, *after* all building destructors have run (so any straggler unregister calls land before the wholesale clear — unregister is cheap so ordering is forgiving).
- **Reset `s_terrainShadowPrimed` on mission unload** if it isn't already — otherwise a second mission load skips priming and relies on the first cam-move to rebuild, masking registration bugs. Confirm current behavior before modifying.

### 7. Plumb the camera-move rebuild signal

Not strictly required in the cache-based design (step 2 already runs on *either* rebuild trigger because it's inside the shared `if (rebuild)` scope), but worth hardening:

At `txmmgr.cpp:1191`, when the `camMovedFar > 100²` branch is taken, also set a "static props need redraw this frame" latch. Currently unused, but harmless, and prevents a future refactor from splitting the wall pass out of this scope without noticing.

## Test plan

1. **Walls still cast shadow at all.** Start any mission with walls. Shadows appear identical to current behavior (actually marginally *better* — static light matrix instead of dynamic follow).
2. **Off-screen walls cast on-screen shadows.** Position camera so a wall is just off the bottom-of-screen at sunset angle. Its shadow should reach across visible terrain. Impossible under the 1200-unit dynamic follow-radius — primary quality win.
3. **Camera-move invalidation.** Pan camera >100 world units. Static shadow re-accumulates terrain + walls coherently (no ghosted wall shadow at the old camera position).
4. **Destruction invalidation.** Destroy a wall with a mech. Shadow disappears within 1–2 rebuild cycles.
5. **Performance check.** Tracy `Shadow.DynPass` batch count measurably smaller on wall-heavy maps (airfield especially). New `Shadow.StaticProps` zone appears on rebuild frames only.
6. **Turret / non-wall building regression.** Any building without `BUILDING_SUBTYPE_WALL` casts shadows exactly as before.
7. **Mission reload.** Load mission A → exit to main menu → load mission B. `gos_GetWallShadowLiveCount()` returns to 0 between missions; B's walls cast correctly. If `s_terrainShadowPrimed` isn't reset, B fails — guard against regression.
8. **Static shadow overlay.** RAlt+F2 during a rebuild. Walls appear in the static depth map.
9. **Duplicate-registration guard.** Force a double-register (dev build only: temporarily call `registerStaticShadow` twice in `Building::init`). Live count should equal single-register count. If LOD/damage swap gets added later, this is the regression test.
10. **Slot recycling under churn.** Script scenario: destroy 100 walls, spawn 100 replacements. Confirm `gos_GetWallShadowHighWater()` stays bounded while live count cycles. Without free-list recycling, high-water grows unbounded.
11. **Stale-resource after abort.** Load mission → abort mid-load or via ESC → load different mission. No GL errors, no flicker from stale cache entries.
12. **Shadow-pass state parity.** Visual check: no acne on wall faces, no peter-panning at wall base where wall meets terrain shadow. These are the signatures of bias mismatch between the static terrain pre-pass and the static wall pass.
13. **Shape-swap regression (unregister-then-add contract).** Dev-only: force a wall to re-register with a different mesh (fake a damage-state swap by calling `registerStaticShadow` a second time with mutated shape data). After the next rebuild, only the new geometry should cast — the old shape's shadow must disappear. Directly exercises the idempotent Begin/Add/End transaction.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| **Owner-ID reuse within a mission** | Catastrophic if the prerequisite is wrong: a new wall reusing a dead wall's ID would unregister/overwrite living cache entries belonging to another live owner. Prerequisite section blocks implementation until verified. Keep a debug-build log that records `(ownerId, Building*)` on every register and asserts the pair is unique across a mission. |
| Cull-gate cascade on walls (per load-bearing memo) | Cache decouples shadow submission from cull/render entirely. Wall render path unchanged. |
| `MAX_WALL_SHADOW_ENTRIES` exhaustion | 2048 entries + free-list recycling. Exhaustion now bounded by live wall count, not lifetime churn. Log WARN on register if `gos_GetWallShadowFreeCount() == 0 && g_numWallShadows == MAX`. |
| Wall spawned mid-mission after prime | `registerStaticShadow` unconditionally calls `gos_RequestFullShadowRebuild()` — late walls never silently miss the accumulator. See "Initialization contract" in step 3. |
| Wall's `worldMatrix` captured stale | Walls immobile — matrix at load is final. Debug-only: assert subtype is `BUILDING_SUBTYPE_WALL` AND comment that *immobility is an empirical engine/content assumption, not guaranteed by subtype alone*. If a modded asset uses wall subtype with nontrivial transform behavior, shadow will silently desync. Document in `memory/` so broadening revisits this. |
| Mid-mission shape swap (LOD / damage) not reflected | Swap sites must call `registerStaticShadow(ownerId)` again. Idempotent contract handles this cleanly. Audit step 3a enumerates swap paths. |
| Compound BldgAppearance with multiple shapes | Multi-primitive owner is first-class: register appends N entries under same `ownerId`, unregister removes all of them. |
| Dangling `vb/ib/vdecl` after non-destruction teardown | Unregister is called from both destruction sentinel AND universal destructor (step 5). Double-unregister is a no-op. |
| Global-suppress latch swallows unrelated shadow submissions | Replaced with explicit `skipShadowSubmit` parameter threaded through `TG_Shape::Render` (step 4). No ambient state. |
| `s_terrainShadowPrimed` not reset on mission unload | Test 7 guards this. If unset, second mission misses prime rebuild. Fix in step 6. |
| Static pass state mismatch (bias/cull/matrix) vs terrain | State parity checklist in step 2. Test 12 (acne / peter-panning) catches regressions. |
| `baseTileId` sentinel skipping walls | Confirmed: bldng.cpp:827 has no subtype gate, fires for any destroyed building. |
| GL context loss invalidating cached buffers | MC2 doesn't recover from context loss (process exits). Not handled. Documented in memory. |

## After walls work, broadening

Promote more building subtypes by adding a second call site for `gos_RegisterWallShadow` (rename then — `gos_RegisterStaticPropShadow`). Ordered by safety:

1. Hangars / depots with no rotational node (`S_stricmp(appearType->rotationalNodeId, "NONE") == 0 && !appearType->spinMe`) — the `spinMe`/rotationalNode check is *dead scaffolding* for walls but becomes load-bearing here.
2. Generic buildings passing the same check.
3. Bridges (landbridges) — tricky because destruction changes terrain overlay under them.

Turrets stay out permanently (rotational nodes animate per-frame → matrix isn't cacheable) until we do the shape-hierarchy split that separates turret base from barrel.

When broadening, *also* revisit whether the `SHADOW_COLLECT_*` mode infrastructure (deferred from v1) is now needed — it becomes worth building if we find a class of props that move occasionally but not per-frame.

## Estimated effort

- Steps 1–2: 3h (cache + free list, static pass hook, state-parity audit)
- Step 3 + 3a: 2h (registration, swap-site audit, lifetime audit)
- Step 4: 1.5h (skipShadowSubmit parameter through TG_Shape::Render)
- Step 5: 1h (unregister on destruction + universal teardown)
- Step 6: 0.5h (mission lifecycle + prime reset)
- Step 7: 0.5h (signal hardening)
- Testing (13 scenarios including stress/regression): 3–4h
- **Total: 11–13h + debugging buffer**

## Rollback

Every change is additive:
- `g_wallShadows` cache + APIs — new code, unreferenced if `registerStaticShadow` is never called.
- `BldgAppearance::registerStaticShadow` — opt-in; disable by not calling from `Building::init()`.
- `TG_Shape::Render` gets a defaulted `skipShadowSubmit=false` parameter — no caller behavior changes until `BldgAppearance` starts passing `true`.
- Static wall pass in `txmmgr.cpp` — gated on `gos_GetWallShadowLiveCount() > 0`; zero effect if empty.

Disabling by skipping step 3's registration call reverts to current behavior completely. No schema changes, no save-file changes.
