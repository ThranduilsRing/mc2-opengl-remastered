# GPU static-prop path — cull infrastructure lessons

Written after the 2026-04-20 sessions that landed commits `f0b85f0`
through `e23746c` on branch `claude/nifty-mendeleev`. Covers the
traps anyone touching visibility, props, or the D3D→GL projection
chain needs to know.

## Problem statement

The wolfman zoom ("pulled-out strategy view") has a long-standing
bug: **most buildings and half the mechs don't render**, though they
ARE in the level. The handoff describes a ~87% false-negative rate on
the `inView` angular cull at wolfman zoom. The intent of the GPU
static-prop renderer (RAlt+0 killswitch) was to bypass that cull —
have the GPU's own clipper decide what's visible, trusting the
hardware instead of the broken screen-space math.

Multiple sessions have investigated. The core cull math in
`mclib/terrain.cpp:1040-1053` and `mclib/bdactor.cpp:1090-1167`
(recalcBounds) is intricate enough that nobody has cracked it.

This doc is about the **secondary problem**: every bypass attempt
cascades into a different failure because the cull gates are
load-bearing for much more than visibility.

## The cull chain, top to bottom

```
 terrain.cpp:geometry() — per-terrain-vertex angular cull
  → terrain.cpp:1127 if (clipInfo) setObjBlockActive + setObjVertexActive
    → objmgr.cpp:1488 iterate terrain objects (update pass)
      │   gated by objBlockInfo[block].active
      │   inner gated by objVertexActive[obj->getVertexNum()]
      ├→ objList[i]->update()  ← stale-state refresh
      │     → appearance->update() (BldgAppearance etc)
      │        → recalcBounds() sets inView per-actor
      │        → if (inView || killswitch) TransformMultiShape
      │          → TG_Shape::TransformShape
      │            → getVerticesFromPool(N) — NULL on exhaustion
      │            → returns 1, listOfVertices may be NULL
      │        → if (update() returned false) setExists(false)
      │
    → objmgr.cpp:1486 iterate terrain objects (render pass)
      │   same gates
      ├→ objList[i]->render() (Building::render etc)
      │     → if (appearance->canBeSeen() || killswitch)
      │        → appearance->render() (BldgAppearance::render)
      │          → if (inView || killswitch)
      │             → if (GPU) submitMultiShape(...)
      │             → else bldgShape->Render()
      │                → TG_Shape::Render
      │                  → if (!listOfVertices || !listOfColors ||
      │                        lastTurnTransformed != turn-1) return
      │                  → glDrawElements...

 Movers (mechs, vehicles) loop separately:
 objmgr.cpp:1527 mechs[i]->render()
  → BattleMech::render
    → if (Terrain::IsGameSelectTerrainPosition(pos))
      → friendly-vs-enemy branch
      → appearance->render() (Mech3DAppearance::render)
        → if (inView || killswitch)
          → mechShape->Render
            → same pool/state guard as above
```

## Five ways the gates are load-bearing

### 1. Update iteration

`GameObjectManager::update` (code/objmgr.cpp:1731) iterates only
`objBlockInfo[block].active` blocks AND checks
`objVertexActive[obj->getVertexNum()]` per object. Out-of-block
objects **never get update() called**. Their per-frame state —
position, rotation, transforms — stays from the last frame they were
active (possibly many frames ago).

### 2. Object lifecycle

`Building::update()` / `Gate::update()` / terrain-object update()
return `false` when their internal sanity checks fail on stale or
invalid state. When that happens, `objmgr.cpp:1748` calls
`setExists(false)`. **The object is permanently destroyed** for the
rest of the mission — not just skipped for this frame.

Bypass attempt: "force all objBlockInfo active before update()".
Regression: update() runs on stale objects, some return false, those
objects are deleted. Visible buildings count goes DOWN, not up.
(Tested and reverted — commits `b4cc927` + `27a1434`.)

### 3. TGL vertex pool budget

The TGL pool is shared across all TG_Shapes drawn per frame. Init'd
in `code/mission.cpp:3097` (now 500K after the `4888084` bump).
`getVerticesFromPool(n)` (mclib/tgl.h:1022) returns NULL when
exhausted. `TransformShape` stores NULL in `listOfVertices`.
`TG_Shape::Render` (mclib/tgl.cpp:2536) silently returns on null.

Render order per frame:
1. `objmgr::update` runs TransformShape for buildings/trees in
   active blocks (most vertex consumption)
2. `objmgr::update` runs TransformShape for mechs (the rest)
3. Render pass draws whatever was transformed

Pool sized for post-cull consumption → fine. Pool sized for pre-cull
(bypass attempt) → ran dry during buildings, mechs got NULL, half
the mechs vanished. Not from the cull — from pool exhaustion. The
signature: mechs disappear "intermittently" / "half the time" while
health bars still draw (bars come from screenPos, updated
separately).

Fixed by bumping the pool (`4888084`). Root-cause understanding is
what makes it non-obvious.

### 4. Per-instance transform freshness

`Mech3DAppearance::update` at mech3d.cpp:4170 has:
```c
if ((turn < 3) || inView || (currentGestureId == GestureJump))
    updateGeometry();
```

`updateGeometry()` runs `mechShape->TransformMultiShape`. Without
it, `mechShape`'s children have stale `listOfVertices` from last
time the mech was in view. `TG_Shape::Render` silent early-outs on
stale data (`lastTurnTransformed != turn-1`). Mech model disappears,
but health bars still draw.

GVAppearance (gvactor.cpp:2702) has the identical pattern.

Fixed in both by adding `|| g_useGpuStaticProps` to the gate
(`aef7e14` for mech, `e23746c` for GV). They still CPU-render (no
GPU submit for animated units), but transforms are fresh.

### 5. D3D-style projection wants no-negative-w

The GPU static-prop vertex shader (static_prop.vert) does manual
perspective divide:
```glsl
vec4 clip4 = u_worldToClip * world;
float rhw  = 1.0 / clip4.w;
vec3  px;
px.x = clip4.x * rhw * u_terrainViewport.x + u_terrainViewport.z;
px.y = clip4.y * rhw * u_terrainViewport.y + u_terrainViewport.w;
px.z = clip4.z * rhw;
vec4 ndc   = u_mvp * vec4(px, 1.0);
float absW = abs(clip4.w);
gl_Position = vec4(ndc.xyz * absW, absW);
```

When `clip.w <= 0` (vertex behind the near plane), `rhw` flips sign,
`px` reverses, and `abs(clip.w)` hides the issue in the w
component. The vertex projects to garbage NDC. If one triangle
vertex is behind camera and two in front, the triangle spans the
entire screen as diagonal streaks.

CPU path avoids this because CPU pre-culls out-of-view objects.
GPU path under the killswitch submits everything, so we see these
streaks at specific camera angles.

Guard in commit `ea96c13`: `if (clip4.w < 0.1) gl_Position =
vec4(2,2,2,1);` (outside standard clip volume → OpenGL discards it).

Terrain overlay vert (shaders/terrain_overlay.vert) uses the exact
same math and has the same vulnerability. It doesn't hit this in
practice because terrain vertices are always in front of the
camera (we're looking down at them), but the pattern is identical.

## The load-bearing TL;DR

For anyone changing the cull, the renderer, the static-prop batcher,
TGL pools, the D3D→GL projection chain, or per-actor render/update
code paths:

| If you change this | You may also need to change |
|---|---|
| Cull gate (inView, canBeSeen) | TGL pool size (pre-cull budget) |
| Pool size | tglHeap size (`mission.cpp:3091`) |
| Bypass active-block iteration | The update path too, or state goes stale |
| Force objects to update | Accept that some will `setExists(false)` |
| D3D-style clip.w math | Guard for w <= 0 (ea96c13 pattern) |
| Per-vertex read source | `listOfVertices[j].argb`, never listOfColors |
| Texture handle | Resolve at draw time, mutated every frame |

## What fully works as of session end (commit `593dba5`)

**CPU path (killswitch OFF, default):** Perfect — vanilla behavior
preserved. Pool bump is non-regressive (bigger pool, same allocation
pattern).

**GPU path (killswitch ON):** Acts as "static-props-off". All the
enabling fixes ARE present (Building/TerrainObject/Gate/Artillery
bypass canBeSeen, Mech/GV bypass inView, GPU submit logic, shader
guards), but the combined behavior ends up hiding props rather than
rendering them through the GPU path. Useful as a "clean terrain"
toggle.

## What still doesn't work and why

1. **GPU path doesn't render the props it was supposed to.** Needs
   a separate iteration loop that doesn't share `ObjectManager`
   infrastructure with the CPU path, OR the terrain-vertex angular
   cull needs to be fixed at its source.

2. **Late registerType (2 types per mission).** Types registered
   after `finalizeGeometry()` are rejected. Source suspected to be
   artillery/bomber spawns (artlry.cpp:1580 "Shilone"). Fixing needs
   re-runnable `finalizeGeometry` with incremental VBO upload.

3. **Shadows (Tasks 13-14).** `flushShadow()` is empty. Would need
   a depth-only GPU pass wired into `Shadow.DynPass`.

4. **GVs/Mechs not on GPU submit.** Per-node animation requires
   per-instance bone matrices in the SSBO; current batcher is
   T-pose only.

## References

- Handoffs:
  `docs/superpowers/plans/progress/2026-04-19-static-prop-handoff.md`,
  `docs/superpowers/plans/progress/2026-04-20-static-prop-handoff.md`,
  `docs/superpowers/plans/progress/2026-04-20-static-prop-handoff-part2.md`
- Memory files (auto-loaded cross-session):
  `cull_gates_are_load_bearing.md`, `tgl_pool_exhaustion_is_silent.md`,
  `mc2_argb_packing.md`, `mc2_texture_handle_is_live.md`,
  `static_prop_projection.md`, `clip_w_sign_trap.md`,
  `terrain_tes_projection.md`

## Commits summary (branch `claude/nifty-mendeleev`)

```
e23746c fix(props): extend inView/canBeSeen bypass to GV, Gate, Artillery
ea96c13 fix(props): guard behind-camera vertices
4888084 fix(props): bump TGL pools (30K->500K) — fixes silent mech drop-outs
aef7e14 fix(props): bypass mech-geometry cull (Mech3DAppearance::update)
2a71a41 fix(props): bypass Building/TerrainObject canBeSeen()
4ee3783 feat(props): wire Tree + Generic to GPU submit path
4af44f7 fix(props): read argb from listOfVertices (not listOfColors);
        resolve texture handle at draw time (not registerType)
1585db1 fix(props): skip ineligible multishape children (don't fail whole)
f0b85f0 feat(props): RAlt+9 frag debug-mode cycle
```

Reverts (don't redo):
```
27a1434 revert force-active blocks (destroyed objects via setExists)
4c53937 revert objmgr block-cull bypass (streaks)
```
