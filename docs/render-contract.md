# MC2 OpenGL Render Contract

This document defines the authoritative rendering contracts for the modernized
OpenGL renderer. Its purpose is to stop accidental mixed-state paths from
spreading through the codebase and to make future rendering work easier to
reason about.

Use this file when changing:
- coordinate spaces
- submission formats
- culling and visibility decisions
- shader ownership of projection or depth
- overlay, water, terrain, object, and UI render paths

If a proposed change does not fit one of the contract buckets below, the change
is probably introducing a new bridge state and should be reconsidered.

## Core Rules

### Rule 1: Every render path has one authoritative submission space

A path must be one of:
- **world-space authoritative**
- **projected-space authoritative**
- **screen-space authoritative**

Do not allow one path to submit vertices in one space while making correctness
decisions in another space unless the path is explicitly documented as a bridge
path scheduled for removal.

### Rule 2: Visibility ownership must be explicit

For each path, document who owns visibility and clipping:
- CPU world-space visibility
- CPU projected-space visibility
- GPU clip-space visibility
- mixed bridge path (temporary only)

### Rule 3: Shadow ownership must be explicit

For each path, document who is responsible for shadow correctness:
- forward shading in the path's own fragment shader
- post-process bridge path
- not shadowed by design

### Rule 4: Projected exceptions are valid

Not every `projectZ()` call is debt. Some paths are correctly projected by
design. These must remain explicit exceptions rather than being swept into
broad "GPU projection cleanup."

### Rule 5: Bridge code is allowed only with an exit plan

Temporary compatibility layers are acceptable when they unblock migration, but
they must be called out as bridge state in this document and in the design doc
that introduced them.

## Coordinate Space Vocabulary

### Raw MC2 world space

- `x = east`
- `y = north`
- `z = elevation`

This is the preferred authoritative world-space contract for terrain and future
GPU-native world geometry.

### Stuff / camera space

Used by older MC2/MLR math. See [architecture.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/architecture.md).

### Projected space

The result of `Camera::projectZ()` or equivalent CPU projection. This includes
values such as:
- `px`, `py`, `pz`, `pw`
- `wx`, `wy`, `wz`, `ww` for some older projected paths

### Screen-space

Pixel-space UI or pretransformed vertex submission intended to map directly to
the final viewport.

## Contract Buckets

## Bucket A: World-Space Authoritative

These paths submit raw MC2 world-space positions and expect GPU projection and
GPU depth/shadow logic to be authoritative.

### A1. Terrain base (tessellated)

Status:
- active
- partially clean
- still has CPU visibility debt

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)
- [code/gamecam.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gamecam.cpp)
- [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)
- [shaders/gos_terrain.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert)
- [shaders/gos_terrain.tesc](A:/Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\gos_terrain.tesc)
- [shaders/gos_terrain.tese](A:/Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\gos_terrain.tese)

Authoritative submission space:
- raw MC2 world space

Projection owner:
- GPU tessellation path via `terrainMVP` plus viewport chain

Shadow owner:
- forward terrain shading

Current debt:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp) submits world-space terrain vertices but still contains projected-space cull semantics through `pz`
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp) still uses `projectZ()` as the producer of terrain visibility metadata

Required end state:
- world-space submission remains
- CPU visibility becomes coarse world-space visibility rather than projected-depth correctness
- no terrain correctness dependency on `pz`

### A2. Grass

Status:
- aligned with terrain path

Primary files:
- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- [shaders/gos_grass.geom](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_grass.geom)
- [shaders/gos_grass.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_grass.frag)

Authoritative submission space:
- terrain-derived world-space inputs

Projection owner:
- GPU

Shadow owner:
- forward shading

Notes:
- This path should continue to inherit terrain's world-space contract.

### A3. Terrain overlays and decals (target state)

Status:
- target state, not fully implemented

Primary design:
- [2026-04-15-world-space-overlay-design.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/plans/2026-04-15-world-space-overlay-design.md)

Types intended to move here:
- alpha cement / transition overlays
- craters
- footprints
- terrain decals

Authoritative submission space:
- raw MC2 world space

Projection owner:
- GPU, using a typed world-space batch path

Shadow owner:
- forward shading in the dedicated overlay/decal shaders

Required end state:
- no `rhw=1.0` bridge semantics
- no terrain-specific overlay behavior hidden inside generic textured shaders

## Bucket B: Projected-Space Authoritative

These paths are correctly projected by design and should not be migrated merely
for ideological consistency.

### B1. Water surface and water detail

Status:
- intentional projected path
- do not casually convert

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [shaders/gos_tex_vertex.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.vert)
- [shaders/gos_tex_vertex.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.frag)

Authoritative submission space:
- CPU-projected space

Projection owner:
- `projectZ()`

Why it is projected on purpose:
- water projects a wave-displaced point, not just terrain vertex elevation
- the current alpha/edge behavior relies on that projected contract

Migration rule:
- do not migrate water piecemeal
- any future water rewrite must change submission, culling, and shader semantics together

### B2. Picking, cursor anchoring, and explicit screen-related helpers

Status:
- projected or screen-space by purpose

Examples:
- mouse picking support
- screen anchoring
- debugging helpers whose reason for existence is 2D placement

Migration rule:
- keep these projected unless the feature itself is redefined

## Bucket C: Screen-Space Authoritative

### C1. HUD, text, and menu/UI

Status:
- intentional screen-space paths

Primary files:
- UI and text shader paths
- menu and HUD submission code

Authoritative submission space:
- screen-space

Projection owner:
- caller / UI code

Shadow owner:
- none by default

Notes:
- scene post-processing should not implicitly redefine this contract

## Bucket D: Bridge Paths To Remove

These paths are temporary compatibility layers. They are allowed only while a
replacement path is being brought online.

### D1. Terrain world-space submission gated by projected depth

Status:
- active
- highest-priority contract violation

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)

Problem:
- terrain base vertices are submitted in world-space
- triangle acceptance still depends on `pz` values produced by CPU projection

Why this is dangerous:
- it makes terrain correctness depend on a projected-space producer even though
  terrain is visually GPU-driven
- it encourages future half-migrations

Required cleanup:
- remove projected-depth correctness dependence
- replace with a contract-consistent visibility decision

### D2. IS_OVERLAY / `rhw=1.0` / terrainMVP bridge inside `gos_tex_vertex`

Status:
- active bridge
- acceptable short-term
- not end-state architecture

Primary files:
- [shaders/gos_tex_vertex.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.vert)
- [shaders/gos_tex_vertex.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.frag)

Why it existed:
- it enabled world-space-like terrain overlays before a dedicated typed path existed

Why it should be retired:
- it hides terrain-specific semantics inside generic textured rendering
- it makes future maintenance harder

Replacement:
- dedicated typed world-space overlay/decal batches

### D3. Post-process shadow pass for world geometry that should self-shadow

Status:
- bridge / compatibility layer

Primary design:
- [2026-04-11-gbuffer-postprocess-shadow-design.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/plans/2026-04-11-gbuffer-postprocess-shadow-design.md)

Notes:
- the post pass was a valid bridge for legacy projected paths
- it should not remain the main long-term shadow strategy for world geometry

Target state:
- static terrain shadows own terrain macro-lighting
- dynamic local shadow pass owns moving-caster detail
- forward shading handles world-space geometry when possible
- post-process shadowing becomes optional fallback, not architectural center

## Allowed Legacy Containment

Some paths may remain legacy if the payoff from migration is low. These should
be clearly contained and documented rather than repeatedly half-modernized.

Candidates:
- some older object submission paths
- particle paths that are visually acceptable and low-risk
- editor/debug draw helpers

Rule:
- if a path remains legacy, mark it explicitly and do not route new world-space
  renderer features through it

## Migration Policy

### Approved migration order

1. document the path contract
2. classify submission space and visibility ownership
3. introduce a typed replacement path if needed
4. switch one family of draws atomically
5. delete the bridge code only after the replacement is verified

### Disallowed migration pattern

Do not change:
- submission semantics
- cull/visibility semantics
- shader expectations

in separate passes for the same draw family if the path is currently mixed.

That is how silent half-state regressions happen.

## Current Priorities

### Priority 1

Remove terrain's dependency on projected-depth correctness:
- document the exact terrain cull contract
- stop using `pz` as a load-bearing terrain correctness input

### Priority 2

Move terrain-adjacent overlays and decals onto typed world-space batches:
- alpha cement
- craters
- footprints

### Priority 3

Shrink the role of bridge shadow paths:
- rely less on post-process shadowing for world geometry
- keep projected exceptions explicit

## Non-Goals

These are not goals of the render contract cleanup:
- removing every `projectZ()` call in the codebase
- forcing water into the terrain world-space contract without a full redesign
- converting UI or picking to world-space for aesthetic consistency

## Review Checklist

When reviewing any future rendering change, ask:

1. What is the authoritative submission space?
2. Who owns visibility?
3. Who owns depth correctness?
4. Who owns shadows?
5. Is this a stable contract or a bridge state?
6. If it is a bridge, where is the exit plan documented?
