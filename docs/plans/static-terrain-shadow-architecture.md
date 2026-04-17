# Static Terrain Shadow Architecture

## Goal

Make terrain shadows stable, high-resolution, and world-fixed enough that they
feel like part of the map rather than a camera-driven effect.

This design assumes:
- the current post-process shadow pass is a bridge path, not the main long-term
  shadow solution
- terrain is effectively static for the duration of a mission
- startup-time work is acceptable if it removes gameplay hitching and visible
  shadow instability

## Problem Statement

The current shadow system is visually strong but architecturally split:

1. a static terrain shadow map exists, but its content is currently accumulated
   incrementally as the camera moves
2. a dynamic local shadow map exists for moving objects
3. a post-process shadow pass exists as a bridge for older non-world-space paths

This creates three stability problems:

- **camera-triggered terrain shadow work**
  The static terrain shadow map is treated as an incrementally populated cache
  rather than a completed world asset.

- **camera-history dependence**
  Shadow completeness depends on where the camera has already been.

- **blurred ownership**
  The renderer has more than one answer to "where do terrain shadows really come
  from?"

## Architectural Direction

The target design is:

- a **startup-built world-fixed terrain shadow map** provides the baseline
  terrain shadowing for the whole active map
- a **dynamic local shadow map** adds moving-caster detail near the active
  combat/focus area
- the **post-process shadow pass** becomes optional bridge/fallback behavior for
  leftover legacy projected paths rather than the center of the shadow system

This is the stable model:

```text
Mission load:
  build high-resolution terrain shadow atlas once

During gameplay:
  sample terrain shadow atlas everywhere
  build dynamic local shadow map every frame or as needed
  combine static + dynamic in forward shading
  use post-process shadowing only for remaining bridge paths
```

## Why Startup Build Is The Right Trade

Terrain in MC2 is static enough that building its shadow solution once at
mission start is the cleanest architecture.

Advantages:
- no gameplay hitching from terrain shadow refresh
- no dependence on recent camera movement
- easier debugging and reproducibility
- cleaner mental model for future maintainers

Tradeoff:
- increased mission load time

This is a good trade if load-time cost is bounded and gameplay becomes stable.

## Current State Summary

Primary files:
- [mclib/txmmgr.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/txmmgr.cpp)
- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)
- [code/gamecam.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gamecam.cpp)
- [shaders/shadow_terrain.tesc](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/shadow_terrain.tesc)
- [shaders/shadow_terrain.tese](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/shadow_terrain.tese)
- [shaders/include/shadow.hglsl](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/include/shadow.hglsl)

Observed current structure:
- world-fixed light matrix logic exists
- terrain shadow map generation exists
- terrain shadow generation is still treated as an incremental cache in the
  render loop
- dynamic local shadowing exists and should remain

## Design Principles

### Principle 1: Terrain shadow data should not depend on camera travel

If the camera can change the completeness of terrain shadow data, the map is not
actually static from the player's point of view.

### Principle 2: Light-space framing should be stable for the whole mission

Once the mission starts, terrain shadow projection should be a pure function of:
- map bounds
- sun direction
- chosen safety padding

It should not be influenced by current camera position.

### Principle 3: Static and dynamic shadow roles must be separate

Static terrain shadow:
- large-scale terrain/world grounding
- fully stable
- built once

Dynamic local shadow:
- moving object detail
- local coverage around combat/focus area
- allowed to move and update frequently

### Principle 4: Resolution should be spent where stability already exists

There is little value in pushing filter complexity on top of unstable ownership.
First stabilize geometry, map framing, and build timing. Then spend more
resolution and filtering budget.

## Proposed Shadow Ownership Model

## Layer 1: Static Terrain Shadow Atlas

Responsibility:
- all terrain macro shadowing for the active map

Built:
- once at mission load
- rebuilt only on explicit invalidation

Coverage:
- the entire active terrain map

Projection:
- world-fixed orthographic light-space matrix derived from map bounds and sun
  direction

Resolution target:
- significantly higher than the current baseline if memory budget allows

Recommended direction:
- prefer a very high-resolution single world-fixed map first
- only introduce tiled/cascaded variants if a single map proves insufficient

Why this should be first:
- simplest mental model
- easiest to debug
- best fit for finite static RTS terrain

## Layer 2: Dynamic Local Shadow Map

Responsibility:
- moving mechs
- buildings or dynamic casters if needed
- local high-frequency shadow detail around the active battle area

Built:
- per frame or per local update

Coverage:
- camera/focus-centered local region only

Projection:
- local orthographic region tuned for object detail, not world-scale coverage

Notes:
- dynamic shadows should not try to own terrain macro-lighting
- they should complement the static terrain atlas, not replace it

## Layer 3: Post-Process Shadow Bridge

Responsibility:
- temporary support for legacy projected geometry paths that do not yet have
  proper forward shadow ownership

Target role:
- compatibility bridge
- optional fallback
- not core terrain shadow architecture

Long-term expectation:
- its scope shrinks as more paths move onto explicit world-space or typed shadow
  contracts

## Static Terrain Shadow Build Design

### Build timing

Recommended timing:
- mission load / terrain load completion

Not recommended:
- camera-motion-driven incremental accumulation during gameplay

### Inputs to static build

The static terrain shadow atlas should be built from:
- terrain geometry submission path
- stable sun direction
- fixed map bounds
- fixed light-space padding policy
- final terrain displacement semantics used in gameplay shading

### Invalidations

Allowed invalidation triggers:
- mission load
- map change
- explicit sun direction change
- explicit terrain topology/material change that affects shadow silhouette

Not allowed as invalidation triggers:
- normal camera motion
- camera rotation
- camera zoom

## Resolution Strategy

If the goal is "beyond all reason" stability and presence, the static terrain
shadow map should be treated as premium terrain infrastructure.

### Recommended approach

Start with:
- a single world-fixed high-resolution terrain shadow map

If hardware budget permits, increase above the current baseline aggressively.
The exact number can be tuned later, but the design direction should assume that
terrain shadow quality is important enough to spend real memory on.

### Why single-map first

Advantages:
- simplest implementation
- simplest debugging
- easiest visual reasoning
- no cascade transitions or tile seams

Only move to more complex atlasing if:
- a single map cannot retain enough detail across the full mission terrain
- or memory/performance characteristics force a different design

## Stability Requirements

These are the real success criteria for the static terrain shadow system.

### Requirement 1

Translating the camera across the map must not change static terrain shadow
edges.

### Requirement 2

Rotating the camera must not cause terrain shadow swimming or edge drift.

### Requirement 3

Terrain shadow quality must no longer depend on where the camera has already
been during the mission.

### Requirement 4

There must be no gameplay hitch caused by terrain shadow rebuild under normal
camera motion.

### Requirement 5

Static terrain and dynamic local shadows must visually agree rather than looking
like two separate systems.

## Banding And Quality Tuning

After the static atlas is truly stable, tune quality in this order.

### Step 1: Resolution

Increase static map resolution until terrain macro shadows stop feeling blurred
or under-sampled at normal play distances.

### Step 2: Bias stability

Tune:
- depth bias
- polygon offset
- normal-offset use
- slope-related bias behavior

Goal:
- remove acne and false detachment without introducing visible floating

### Step 3: Filtering and sampling

Tune:
- PCF tap count
- Poisson radius
- per-pixel rotation strategy
- softness policy

Goal:
- reduce sampling banding while preserving edge solidity

### Step 4: Material-aware tuning

If needed later, allow terrain materials or distance bands to influence shadow
softness subtly. This is polish, not first-pass architecture.

## Relationship To Terrain Contract Cleanup

Static terrain shadow stability depends on the terrain renderer having a stable
world-space contract.

Specifically:
- terrain shadow generation must consume the same world surface that terrain
  shading uses
- terrain should not rely on projected-space correctness decisions upstream
- typed world-space terrain-adjacent paths reduce disagreement between terrain
  and what sits on top of it

The shadow architecture and terrain contract cleanup should therefore be treated
as parallel efforts, not separate unrelated tasks.

## Recommended Implementation Order

### Phase 1: Demote incremental gameplay accumulation

Change the architecture so the static terrain shadow atlas is conceptually built
at load, not grown as the camera moves.

Deliverable:
- clear ownership model
- no gameplay-triggered terrain shadow cache growth

### Phase 2: Build full-map static terrain shadow at startup

Use mission/map initialization timing to build the complete terrain atlas.

Deliverable:
- one completed world-fixed terrain shadow map before gameplay

### Phase 3: Raise resolution and stabilize framing

Increase resolution and finalize the light-space framing policy based on map
bounds and sun direction.

Deliverable:
- stable, high-detail terrain shadow baseline

### Phase 4: Re-tune dynamic local shadows against the stable baseline

Keep dynamic local shadows focused on moving-caster detail and local quality.

Deliverable:
- static and dynamic shadows feel coherent together

### Phase 5: Reduce reliance on post-process shadow bridge

As more paths gain proper world-space shadow ownership, narrow the scope of the
post-process pass.

Deliverable:
- cleaner final architecture

## Explicit Non-Goals

These are not goals of this document:
- replacing the dynamic local shadow system with the static terrain atlas
- forcing every legacy projected path off the post shadow pass immediately
- adding more shadow effects before the base system is stable

## Review Checklist

When reviewing any future shadow change, ask:

1. Does this make terrain shadows more world-fixed or less?
2. Does this increase or decrease dependence on camera motion?
3. Does this clarify ownership between static, dynamic, and post-process shadows?
4. Does this improve actual stability or only sampling cosmetics?
5. Is this spending complexity where a simpler world-fixed solution would work?
