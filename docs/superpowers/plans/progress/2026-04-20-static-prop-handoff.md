# GPU Static Prop Renderer — Handoff (2026-04-20 early AM)

Prior handoff: `2026-04-19-static-prop-handoff.md`. This session closed
both P0 bugs and extended coverage. Read this doc first, then prior.

## Worktree + build

Unchanged from prior:
- Source: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- Branch: `claude/nifty-mendeleev`
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
- CMake: `"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo`
- NEVER `--config Release`. NEVER `cp -r`. Use `cp -f` + `diff -q`.

## Commits this session (on top of `900a86c`)

```
4ee3783 feat(props): wire TreeAppearance + GenericAppearance to GPU path
4af44f7 fix(props): read per-vertex argb and texture handle from correct live sources
1585db1 fix(props): skip ineligible children, don't fail whole multishape
f0b85f0 feat(props): frag debug-mode cycle hotkey (RAlt+9)
```

All granular; `git revert <sha>` any one cleanly.

## Bugs fixed

### 1. Layer B false positives (`1585db1`)

`submitMultiShape` was returning false (whole-multishape CPU fallback)
on any child that was a helper/bone node, a daytime spotlight
(null listOfVertices), or had null listOfColors. Most buildings have
at least one such child, so the safety net was firing on ~100% of
actors. Fix: skip those children individually (CPU Render does the
same — tgl.cpp:2530). Only unregistered SHAPE_NODE types still fail
the whole multishape.

### 2. Black buildings (`4af44f7`)

Two independent zero-source bugs, both "captured stale data at
registration, replayed forever":

- **ARGB**: was reading `shape->listOfColors` (TG_Vertex:
  fog+redSpec+greenSpec+blueSpec — specular only, usually zero).
  Real per-vertex lit ARGB is in `shape->listOfVertices[j].argb`
  (gos_VERTEX, offset 16). Computed by TransformShape every frame.
- **Texture handle**: packet cached
  `typeShape->listOfTextures[slot].gosTextureHandle` at register
  time. MC2's TransformMultiShape rewrites that handle each frame
  via SetTextureHandle (msl.cpp:1321). Fix: packet stores
  textureSlot; flush() resolves the current handle from
  `type.source->listOfTextures[slot]`.

Product of two zero inputs = black. Mode 3 (WHITE) was the litmus
test that proved the fragment shader was running at all; mode 4/5
bisected which input was bad.

### 3. Tree + Generic coverage (`4ee3783`)

TreeAppearance and GenericAppearance were never calling
`submitMultiShape`. Registration was done in a prior session but the
per-frame submit wasn't. Now wired, mirroring `BldgAppearance::render`.

## Still open

### 1. Late registerType (2 types per map load)

Stderr still shows:
```
[GPUPROPS] late registerType for 0x... -- CPU-fallback for this type
[GPUPROPS] late registerType for 0x... -- CPU-fallback for this type
[GPUPROPS] multi=... child 0: unregistered type 0x... -- CPU-fallback whole multishape
```

Two types miss the map-load walk, and at least one multishape
references one of them (so that multishape is fully CPU-fallback).
Source unknown — not in `BuildingAppearanceType::init` / `TreeAppearance
Type::init` / `GenericAppearanceType::init` (those all call
`registerMultiShape`). Candidates: late-spawned via mission events,
triggered effects, or an appearance type loaded from a different
code path.

To hunt: grep for `new TG_TypeMultiShape` and `LoadTGMultiShape*`
in code that runs after `finalizeGeometry()` (mission.cpp:3015).
Or add an `fprintf` at the top of `registerType` when
`s_geometryFinalized` is true dumping the call stack.

### 2. GVAppearance (ground vehicles)

Deliberately not wired. GVs animate via per-node rotation (torso,
weapon mounts). The static-prop path uses the map-load T-pose only,
so a GV rendered via this path wouldn't reflect current node
orientations. Needs either per-instance per-node matrix data in the
SSBO (large change) or skip GVs entirely (current state).

Registration is wired (`gvactor.cpp:1024`) but no submit call exists.

### 3. Shadow path (Tasks 13-14)

`flushShadow()` is empty (batcher.cpp:815). Need depth-only shader
program + `Shadow.DynPass` wire-up. Separate design problem — not
blocked on anything above.

### 4. Fallback actors mixed with GPU in same scene

When Layer B fires for one multishape (e.g. the known unregistered
type), that multishape renders via CPU while its neighbors render
via GPU. Looks correct visually because both paths now write
matching output, but the depth interactions aren't explicitly
verified. Test by getting close to a mixed scene (CPU tree next to
GPU building) and confirming occlusion/z ordering is correct.

## Reference

### Key files

- `GameOS/gameos/gos_static_prop_batcher.cpp` — batcher main (~850 lines).
  - `submitMultiShape` at line 469 (Layer B, skip-not-fail policy).
  - `submit` at line 404 (reads `listOfVertices[j].argb`).
  - `flush` at line 656. Texture handle resolution at line 772.
- `GameOS/gameos/gos_static_prop_batcher.h` — public API + packet struct.
- `GameOS/gameos/gos_static_prop_killswitch.h` — global toggles, debug-mode accessors.
- `shaders/static_prop.frag` — modes 0..5. Mode 3/4/5 added this session.
- `shaders/static_prop.vert` — D3D->GL projection chain. DON'T TOUCH unless prior session's memory on projection is stale.
- `mclib/bdactor.cpp:1521` — BldgAppearance::render (GPU wired).
- `mclib/bdactor.cpp:3953` — TreeAppearance::render (GPU wired).
- `mclib/genactor.cpp:768` — GenericAppearance::render (GPU wired).
- `mclib/gvactor.cpp` — GVAppearance. Registration wired, submit NOT wired (animation).
- `mclib/txmmgr.cpp:~1340` — flush call site.
- `GameOS/gameos/gameosmain.cpp:138` — RAlt+9 debug-mode cycle.
- `GameOS/gameos/gameosmain.cpp:170` — RAlt+0 killswitch toggle.

### Memory (`~/.claude/projects/A--Games-mc2-opengl-src/memory/`)

- `static_prop_projection.md` ⭐ — D3D->GL chain rules. Unchanged.
- `uniform_uint_crash.md` — use `int` in uniforms.
- `terrain_tes_projection.md` — abs(clip.w) is load-bearing.
- `feedback_subagent_deploy.md` — subagents must deploy.

### Test harness (created this session)

- `A:/Games/mc2-opengl/test-harness/orchestrate.py` — full autonomous loop:
  launch, replay, cycle modes, screenshot each, dump stderr.
- `A:/Games/mc2-opengl/test-harness/black_buildings_filtered.json` —
  recorded menu->mission->camera sequence with deterministic
  single RAlt+0 toggle.
- `A:/Games/mc2-opengl/test-harness/runs/<timestamp>/` — per-iteration
  artifacts (screenshots + stderr + orchestrate.log).
- `README.md` in harness dir.

To run: `python A:/Games/mc2-opengl/test-harness/orchestrate.py`
(~2.5min end-to-end, user must not touch keyboard during replay).

## How to resume

1. `cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev`
2. `git log --oneline -10` — see this session's 4 commits on top.
3. Pick a remaining item above. Late-registerType is highest-value
   next (single multishape of CPU pollution visible in every mission);
   shadows (13-14) is the biggest remaining feature.
4. For any visual change: `python A:/Games/mc2-opengl/test-harness/orchestrate.py`
   will validate without user intervention.

## Don't redo

- Don't read `listOfColors` for per-vertex ARGB (it's specular).
  Read `listOfVertices[j].argb`.
- Don't cache texture handle at registration. Resolve at draw time
  from `type.source->listOfTextures[slot].gosTextureHandle`.
- Don't fail whole multishape on per-child conditions that the CPU
  path also silently skips.
- Don't repurpose RAlt+0, RAlt+9 without noting in commit + handoff.
- Don't wire GVs to static-prop path until per-node animation is
  handled on GPU.
