# Walls to Static Shadow Pass — Implementation Plan

## Goal

Move wall buildings (`BUILDING_SUBTYPE_WALL`) from per-frame dynamic shadow collection into the world-fixed static shadow accumulator. Turret/radar buildings and all other dynamic objects (mechs, vehicles, etc.) stay in the dynamic pass unchanged.

## Why

- Per-frame GPU savings: dozens to hundreds of wall shapes removed from dynamic shadow draw calls every frame.
- Shadow quality: long sunset wall shadows stop clipping at the ~1200-unit dynamic-shadow radius; walls inherit world-fixed 4096² projection.
- First proof of the static-prop pattern before we broaden to hangars/depots.

## Guiding principle — from prior session advisor

> "Do not touch update/cull ownership more than necessary. Keep the gameplay/update path exactly as-is and change only which shadow collection bucket a building goes into. Minimize chance of reawakening the cull-gate / resource-pool mess."

See: `memory/cull_gates_are_load_bearing.md`, `memory/tgl_pool_exhaustion_is_silent.md`, `docs/gpu-static-prop-cull-lessons.md`.

## Infrastructure already in place

- `gos_RequestFullShadowRebuild()` / `gos_ShadowRebuildPending()` / `gos_ClearShadowRebuildPending()` — declared in `gos_postprocess.h`, implemented in `gos_postprocess.cpp`. Camera-move-100-units path already uses this.
- Static shadow pass in `mclib/txmmgr.cpp:1188-1240` renders terrain into `shadowFBO_`, gated by `shadowRebuildForced || camMovedFar`, supports multi-frame accumulation.
- `addShadowShape()` in `mclib/txmmgr.cpp:97` is the single collection call site used by `tgl.cpp:2709`.
- `BuildingType::subType == BUILDING_SUBTYPE_WALL` is set at load in `code/bldng.cpp:308` — clean identifier.
- Building destruction transition: `code/bldng.cpp:827` — `status == OBJECT_STATUS_DESTROYED` check guarded by `baseTileId != 177` sentinel that fires exactly once per destruction. Ideal hook point.

## Steps

### 1. Collection bucket split (txmmgr.cpp)

Add a second static-shadow shape array and a collection-mode flag:

```cpp
// txmmgr.cpp, near g_shadowShapes declaration
enum ShadowCollectionMode : int {
    SHADOW_COLLECT_DYNAMIC   = 0,  // default: push to g_shadowShapes[]
    SHADOW_COLLECT_STATIC    = 1,  // push to g_staticShadowShapes[]
    SHADOW_COLLECT_SUPPRESS  = 2,  // drop on the floor (wall not being re-submitted this frame)
};
static int g_shadowCollectionMode = SHADOW_COLLECT_DYNAMIC;

static const int MAX_STATIC_SHADOW_SHAPES = 512;
static ShadowShapeEntry g_staticShadowShapes[MAX_STATIC_SHADOW_SHAPES];
static int g_numStaticShadowShapes = 0;
static bool g_staticShadowSnapshotValid = false;  // true once walls have been submitted at least once
```

Modify `addShadowShape()`:

```cpp
void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl, const float* worldEntries16) {
    if (g_shadowCollectionMode == SHADOW_COLLECT_SUPPRESS) return;

    if (g_shadowCollectionMode == SHADOW_COLLECT_STATIC) {
        if (g_numStaticShadowShapes >= MAX_STATIC_SHADOW_SHAPES) return;
        ShadowShapeEntry& ss = g_staticShadowShapes[g_numStaticShadowShapes++];
        ss.vb = vb; ss.ib = ib; ss.vdecl = vdecl;
        memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
        return;
    }

    // Default dynamic path — unchanged
    if (g_numShadowShapes >= MAX_SHADOW_SHAPES) return;
    ShadowShapeEntry& ss = g_shadowShapes[g_numShadowShapes++];
    ss.vb = vb; ss.ib = ib; ss.vdecl = vdecl;
    memcpy(ss.worldMatrix, worldEntries16, 16 * sizeof(float));
}
```

Expose setter via `mclib/tgl.h` or similar so bdactor.cpp can call it:

```cpp
void gos_SetShadowCollectionMode(int mode);       // write
int  gos_GetShadowCollectionMode(void);           // read (for restoring)
void gos_ClearStaticShadowShapes(void);           // clear static bucket before rebuild
void gos_InvalidateStaticShadowSnapshot(void);    // flag for next rebuild to re-collect
```

### 2. Static pass: render the static bucket (txmmgr.cpp ~line 1230)

Right after `gos_EndShadowPrePass()` and before `gos_ClearShadowRebuildPending()`:

```cpp
// Static object shadow pass — render walls (and later other static-eligible props)
// into the accumulated static shadow map. Only runs during rebuilds.
if (g_numStaticShadowShapes > 0) {
    gos_BeginStaticShadowObjectPass();  // reuses shadowFBO_, NO clear (accumulate)
    for (int si = 0; si < g_numStaticShadowShapes; si++) {
        gos_DrawShadowObjectBatch(g_staticShadowShapes[si].vb,
                                  g_staticShadowShapes[si].ib,
                                  g_staticShadowShapes[si].vdecl,
                                  g_staticShadowShapes[si].worldMatrix);
    }
    gos_EndStaticShadowObjectPass();
    g_staticShadowSnapshotValid = true;
}
g_numStaticShadowShapes = 0;  // consumed
```

New functions in gos_postprocess: `gos_BeginStaticShadowObjectPass` / `gos_EndStaticShadowObjectPass` mirror the dynamic equivalents but bind `shadowFBO_` and set the static light-space matrix as the uniform.

### 3. Wall eligibility + mode wrapping (bdactor.cpp)

In `BldgAppearance::render()` around line 1554 (before `bldgShape->Render()`):

```cpp
// Static shadow eligibility — walls only for initial rollout
const bool canUseStaticShadow =
    !appearType->spinMe &&
    (S_stricmp(appearType->rotationalNodeId, "NONE") == 0) &&
    m_isWall;  // set from outside — see step 4

int savedMode = gos_GetShadowCollectionMode();
if (canUseStaticShadow) {
    // Submit wall geometry to static bucket only when rebuild needs it;
    // otherwise suppress so we don't re-collect existing shadow map contents.
    const bool needStaticRebuild = gos_ShadowRebuildPending() || !gos_IsStaticShadowSnapshotValid();
    gos_SetShadowCollectionMode(needStaticRebuild ? SHADOW_COLLECT_STATIC : SHADOW_COLLECT_SUPPRESS);
}

bldgShape->Render(/* existing args */);

gos_SetShadowCollectionMode(savedMode);
```

**Why suppress on non-rebuild frames:** wall shadows live in the accumulated static depth buffer. Re-submitting every frame would either blow the MAX_STATIC_SHADOW_SHAPES budget or push duplicate depth into the accumulator.

### 4. Plumbing `m_isWall` from Building to BldgAppearance (bldng.cpp + bdactor.h)

At Building init, after `setSubType(BUILDING_SUBTYPE_WALL)` at bldng.cpp:308, also propagate to the appearance:

```cpp
// In Building::init() or construction, after appearance is set
if (getObjectType()->getSubType() == BUILDING_SUBTYPE_WALL) {
    ((BldgAppearance*)appearance)->setIsWall(true);
}
```

Add `bool m_isWall = false;` + setter to `BldgAppearance` (bdactor.h). No behavior change except the flag exists for step 3.

### 5. Destruction invalidation hook (bldng.cpp:827)

Right at the once-per-destruction sentinel path:

```cpp
if ((baseTileId != 177) && (status == OBJECT_STATUS_DESTROYED))
{
    baseTileId = 177;  // existing guard

    // If we were contributing to static shadow, invalidate so next frame rebuilds.
    if (getObjectType()->getSubType() == BUILDING_SUBTYPE_WALL)
        gos_RequestFullShadowRebuild();

    // ... rest of existing block ...
}
```

### 6. Init-time invalidation

First frame of a mission needs the static bucket populated. Call `gos_RequestFullShadowRebuild()` at the end of mission load (wherever the existing terrain shadow prime happens — search for `s_terrainShadowPrimed`). It should already be true on first frame but explicit is safer.

## Test plan

1. **Walls still cast shadow at all.** Start any mission with walls on the map. Shadows should appear identical to current behavior.
2. **Camera-move invalidation.** Pan camera >100 world units. Walls should re-submit once and shadow should stay correct.
3. **Destruction invalidation.** Destroy a wall with a mech. Shadow should disappear within 1-2 frames of destruction.
4. **Mid-mission wall spawn** (if any missions do this). Shadow should appear on the frame after spawn.
5. **Performance check.** Tracy profiler `Shadow.DynPass` should show measurably smaller batch count on maps with many walls (airfield missions especially).
6. **Turret building regression.** Any building with a turret (e.g. point-defense towers) should still cast shadows exactly as before — they're in dynamic.
7. **Multi-frame accumulation.** Watch the static shadow debug overlay (RAlt+F2) to confirm walls appear in the static depth map.

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Cull-gate cascade on walls (per load-bearing memo) | Don't touch wall culling, `inView`, or `canBeSeen`. Only change the shadow-collection side-channel. Wall render path unchanged. |
| Static shadow buffer exhaust | `MAX_STATIC_SHADOW_SHAPES = 512` — budget check: how many walls in the heaviest mission? Probably 80-150. Fine. If exceeded, bump to 1024. |
| Wall appears between rebuilds | Won't — walls are mission-loaded, not spawned. If any mission spawns walls mid-mission, fallback is force-invalidate on any wall spawn event. |
| Rebuild cost spike on wall destruction | Static shadow re-accumulates over multiple frames. Existing camera-move stutter is the baseline. Wall destruction happens rarely enough that the occasional re-render is fine. |
| Wall shape is child of a larger bldgShape | Not a concern — walls are their own BldgAppearance instances, not sub-shapes of a compound. |

## After walls work, broadening

Promote more building subtypes by relaxing the eligibility check in step 3. Ordered by safety:
1. Hangars / depots with no rotational node
2. Generic buildings with `rotationalNodeId == "NONE"` and `!spinMe`
3. Bridges (landbridges) — tricky because of destruction changing terrain overlay

Turrets stay out permanently (or until we do the shape-hierarchy split).

## Estimated effort

- Steps 1-4: 3-4h of coding
- Step 5-6: 1h
- Testing (step 7): 2-3h
- **Total: 6-8h + debugging buffer**

## Rollback

Every change is additive — bucket-aware `addShadowShape`, opt-in static mode in BldgAppearance, opt-in invalidation hook in Building destruction. Disabling by setting `m_isWall = false` everywhere reverts to current behavior completely. No schema changes, no save-file changes.
