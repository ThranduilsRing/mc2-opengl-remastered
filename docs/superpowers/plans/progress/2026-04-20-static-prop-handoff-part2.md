# GPU Static Prop Renderer — Handoff Part 2 (2026-04-20 late AM)

Supersedes `2026-04-20-static-prop-handoff.md` (committed at 27df331).
Read that first for context, then this.

## Session commits (on top of the earlier handoff)

```
ea96c13 fix(props): guard behind-camera vertices to prevent screen-spanning streaks
4888084 fix(props): bump TGL pools (30K->500K) to prevent mech drop-outs
27a1434 Revert "fix(props): force all objBlocks/objVertices active..."
b4cc927 (reverted) fix(props): force all objBlocks/objVertices active...
4c53937 revert: roll back objmgr.cpp block-cull bypass
aef7e14 fix(props): bypass upstream terrain-block + mech-geometry culls
2a71a41 fix(props): bypass Building/Terrain canBeSeen() cull when GPU path on
```

Net-effective changes on top of the earlier handoff:

- `code/bldng.cpp` — Building::render bypasses `canBeSeen()` cull
  when killswitch on
- `code/terrobj.cpp` — TerrainObject::render same bypass
- `mclib/mech3d.cpp` — Mech3DAppearance::update unconditionally
  runs `updateGeometry()` under killswitch; render() bypasses
  inner `inView` check
- `code/mission.cpp` — TGL pool sizes raised (30K → 500K vertex/
  color/shadow, 20K → 200K triangle, 40K → 200K face), tglHeap
  raised 40MB → 128MB
- `shaders/static_prop.vert` — behind-camera guard: if clip4.w < 0.1,
  push gl_Position outside clip volume

## Problem discovery chain this session

User reported "half the buildings missing, half the mechs missing"
at the wolfman (far zoom) view. Investigation traced five distinct
culling/resource gates all contributing:

1. **`canBeSeen()` gate at `Building::render` / `TerrainObject::render`**
   — returns `inView`, which has the handoff's documented 87%
   false-negative rate at wolfman zoom. Fixed by adding
   `|| g_useGpuStaticProps` to each site.

2. **`updateGeometry()` `if (inView)` inside `Mech3DAppearance::update`**
   (mech3d.cpp:4170) — out-of-view mechs never had
   `TransformMultiShape` run, so `mechShape->Render` silently
   returned on stale `listOfVertices`. Visible symptom: mech
   health bar floats with no model underneath. Fixed by adding
   `|| g_useGpuStaticProps` to the gate.

3. **Inner `if (inView)` in `Mech3DAppearance::render`** — same
   thing, gated the actual CPU render call on the stale mech.
   Fixed the same way.

4. **`ObjectManager::render` terrain-block cull** (objmgr.cpp:1486)
   — only iterates terrain objects inside "active" terrain
   blocks, where active-ness is derived from per-terrain-vertex
   onScreen classification (mclib/terrain.cpp:1127). Whole
   hangars/warehouses were never reaching `Building::render` at
   all. **Bypass attempts caused regressions** (stretched-triangle
   streaks when rendering objects with uninitialized state;
   destroyed object instances when `update()` failed sanity
   checks on stale data and called `setExists(false)`). Eventually
   reverted — this gate was left in place.

5. **TGL `vertexPool` exhaustion** (tgl.h:1022) — the ROOT cause
   of "half the mechs missing". The pool returns NULL when
   exhausted; `TransformShape` stores that, `TG_Shape::Render`
   silently returns on null `listOfVertices`. Under the killswitch
   we force-transform more shapes than vanilla (all in-block
   buildings/trees regardless of individual inView), so the 30K
   vertex pool ran dry during the buildings/trees pass, and the
   later mech pass got NULLs → half the mechs silently vanished.
   Fixed by bumping the pool to 500K (and tglHeap to 128MB to
   hold it).

## Key insight for future work

The cull gates weren't just a correctness issue — **they were
load-bearing for shared-resource budgets**. The vanilla game
could run with a 30K vertex pool because the broken cull kept
~87% of out-of-view shapes from ever calling
`getVerticesFromPool`. "Bypass the cull to fix visibility" and
"keep the pool small" are not independently choosable.

Bumping the pool size is a band-aid; the proper architectural
fix would be for the GPU static-prop path to own its own vertex
staging that doesn't share the CPU renderer's pool. Out of scope
for this session.

## Remaining known issues

### 1. Stretched triangles at some camera angles (ea96c13 *may* fix)

User reported large diagonal screen-spanning streaks at "certain
camera angles" under killswitch-on. The shader guard in ea96c13
pushes vertices with `clip4.w < 0.1` outside the clip volume so
OpenGL clips them out. My orchestrator screenshots don't
reproduce the exact angle where the streaks appeared, so I can't
self-verify. If streaks persist after ea96c13, next step is to
increase the threshold (0.1 → 1.0 or more), or to add per-vertex
culling via a discard/clip distance that catches a wider range
of degenerate cases.

### 2. `ObjectManager` block-cull still in place

Bypass attempts caused regressions (streaks + destroyed objects).
The cleanest fix path forward: make a NEW object-iteration loop
specifically for GPU submit that doesn't depend on block-active,
runs separately from the CPU render loop, and does its own
per-object update of state. Big change.

Alternative: fix the terrain-vertex angular cull
(terrain.cpp:1040-1053) so it doesn't have the 87% false-negative
at wolfman zoom. Safer in scope but requires deep understanding
of the cull math and eye projection state.

### 3. Late registerType (2 types per map load)

Unchanged from earlier handoff. Stderr still shows:
```
[GPUPROPS] late registerType for 0x... -- CPU-fallback for this type
[GPUPROPS] multi=0x... child 0: unregistered type ... -- CPU-fallback whole multishape
```
One multishape per mission falls back fully to CPU because at
least one of its children is registered AFTER `finalizeGeometry()`.
Source still not traced.

### 4. GVAppearance (ground vehicles) and MechAppearance not wired to GPU

Not a new issue — same deferred status as the previous handoff.
These use per-node animation that the current static-prop batcher
doesn't support (T-pose only).

### 5. Shadow path (Tasks 13-14)

Unchanged. `flushShadow()` is still empty. The GPU-rendered
buildings don't receive dynamic shadows from mechs/units the way
CPU-rendered ones do.

## Test harness state

- `A:/Games/mc2-opengl/test-harness/orchestrate.py` — autonomous loop.
  Uses `gpu_buildings_filtered.json` (newest user recording,
  filtered to a deterministic single RAlt+0 toggle).
- `A:/Games/mc2-opengl/test-harness/shot_initial.py` — captures
  screenshots right after mission load, before camera panning,
  for the initial-zoom-out view where the broken cull is most
  visible.
- `A:/Games/mc2-opengl/test-harness/runs/<timestamp>/` — per-run
  artifacts (screenshots + stderr).

Debug hotkey modes (RAlt+9 cycles 0..7):
  0 normal
  1 addr-gradient
  2 addr-hash
  3 WHITE — proves fragment shader running
  4 ARGB-only — isolates per-vertex lighting stream
  5 TEX-only — isolates texture sampling
  6 HIGHLIGHT-only — isolates v_highlight contribution
  7 TEX+HIGHLIGHT — no v_argb

## How to resume

1. `cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
2. Read the earlier handoff (`2026-04-20-static-prop-handoff.md`)
   for the first-session context, then this doc.
3. `git log --oneline ea96c13..HEAD` for any work past this doc.
4. User's current pain point is streaks at specific camera angles.
   If ea96c13's guard didn't help, widen the threshold or add
   geometry-shader triangle clipping.

## Don't redo

All the don'ts from the earlier handoff still apply, plus:

- **Don't bypass the ObjectManager block-cull without also
  solving the update/state-init problem.** Either destroys
  objects (update returns false → setExists(false)) or renders
  them with stale matrices (streaks).
- **Don't reduce TGL pool sizes below 500K vertices** unless the
  GPU path's transform-everything behavior is reverted.
- **Don't remove the behind-camera guard in static_prop.vert**
  even if the immediate streak is fixed — it prevents an entire
  class of D3D-port degeneracies.
