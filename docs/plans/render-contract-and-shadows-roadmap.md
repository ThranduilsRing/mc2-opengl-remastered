# Render Contract And Shadow Stability Roadmap

## Purpose

This roadmap turns the architectural direction in:
- [render-contract.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/render-contract.md)
- [static-terrain-shadow-architecture.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/plans/static-terrain-shadow-architecture.md)

into a practical implementation sequence.

It is intentionally biased toward:
- correctness over novelty
- stable world behavior over bridge-layer cleverness
- small safe slices over broad "remove old system" pushes

This roadmap assumes:
- no immediate broad `projectZ()` purge
- water remains a projected exception
- post-process shadowing is now a bridge/fallback, not the target end-state
- terrain is the top architectural priority for contract cleanup

## Program Goals

### Goal 1

Make the terrain renderer contract explicit and internally consistent.

### Goal 2

Move terrain-adjacent overlays/decal-like paths onto typed world-space batches.

### Goal 3

Make static terrain shadows startup-built, world-fixed, and stable.

### Goal 4

Reduce dependence on bridge shadow paths over time.

## High-Level Phases

1. contract baseline and instrumentation cleanup
2. typed terrain overlay/decal path
3. terrain cull/visibility contract cleanup
4. startup-built static terrain shadow map
5. static shadow quality pass
6. dynamic shadow reconciliation
7. post-shadow bridge reduction

## Phase 1: Contract Baseline And Cleanup

### Objective

Create a clean starting point before any behavior change:
- stable docs
- accurate classification
- no stale cleanup targets
- no misleading debug-era state

### Why first

Without this phase, later sessions will keep mixing old assumptions with new
paths and the work will fork into multiple incompatible mental models.

### Deliverables

- [render-contract.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/render-contract.md) becomes the canonical rulebook
- stale references to removed experimental plumbing are corrected in future task
  prompts and design notes as needed
- terrain, overlay, water, and shadow ownership are described consistently

### Code changes

Minimal or none. This is mostly a documentation and task-definition phase.

### Files likely touched

- `docs/render-contract.md`
- `docs/plans/2026-04-16-terrain-worldspace-vertex-design.md`
- `docs/plans/2026-04-16-terrain-worldspace-vertex-impl.md`
- future session prompts / planning docs

### Validation

- future tasks no longer mention `undisplacedDepthMode` as a live target
- task prompts distinguish projected exceptions from bridge debt

### Risk

Low.

## Phase 2: Typed Terrain Overlay / Decal Path

### Objective

Move terrain-adjacent overlay-like content out of the generic textured bridge
path and into dedicated typed world-space submission.

### Why this phase is next

This is the highest-leverage cleanup because it removes terrain-specific bridge
logic from generic rendering and establishes the batch pattern needed for later
world-space cleanup.

### Scope

Start with:
- alpha cement / perimeter overlays
- bomb craters
- mech footprints

Do not include:
- water
- HUD markers
- UI anchoring

### Deliverables

- dedicated terrain overlay batch
- dedicated decal batch
- dedicated shaders
- world-space submission for those paths
- render order clarified around terrain base / overlays / decals

### Primary files

- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/crater.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/crater.cpp)
- [mclib/txmmgr.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/txmmgr.cpp)
- [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)
- `GameOS/gameos/gameos_graphics.h` or equivalent batch declarations
- `shaders/terrain_overlay.vert`
- `shaders/terrain_overlay.frag`
- `shaders/decal.frag`

### Safe first slice

Implement the typed batch plumbing and shader path for one family first:
- alpha cement perimeter only

Do not migrate craters and footprints in the same first commit.

### Suggested commit breakdown

1. add world-space overlay vertex type and batch plumbing
2. add terrain overlay shader pair and renderer draw path
3. route alpha cement overlays to the new batch
4. validate and then migrate crater/decal families one by one
5. delete obsolete `IS_OVERLAY` terrain-specific bridge code only after all
   three families are stable

### Validation

Check:
- perimeter cement aligns with terrain surface
- no black/transparent overlay regressions
- no mission-marker contamination
- no water behavior changes
- depth ordering is stable at shallow camera angles

### Risk

Medium.

Primary risk:
- accidentally migrating a non-terrain overlay or marker that relied on the old
  generic path

### Explicit deferral

Do not remove `gos_tex_vertex` overlay bridge code in the same commit that
introduces the new batch. Keep both paths until migrated families are proven.

## Phase 3: Terrain Cull / Visibility Contract Cleanup

### Objective

Remove terrain's current dependency on projected-depth correctness while keeping
terrain visually stable.

### Why this is phase 3, not phase 2

The current terrain path is still delicate. Typed terrain-adjacent batches give
you a cleaner environment before touching the terrain cull contract itself.

### Deliverables

- terrain submission and terrain acceptance logic become contract-consistent
- `pz` is no longer a load-bearing correctness input for terrain draw acceptance
- the next step toward world-space visibility ownership is unblocked

### Primary files

- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)
- possibly [code/gamecam.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gamecam.cpp) if visibility metadata ownership changes

### Safe first slice

The first code slice here should be narrowly scoped and explicitly labeled as a
contract cleanup slice, not a full GPU projection migration.

Candidate safe slice:
- replace terrain acceptance logic in `quad.cpp` with a contract-consistent path
  only if validation criteria are defined first

Required validation focus:
- near-camera terrain triangles
- horizon-edge terrain
- cement/road overlays that nest inside the terrain gate
- terrain-on / terrain-off kill switch behavior
- no water changes

### Next slice after that

Once the immediate `quad.cpp` mismatch is resolved and verified, the deeper
cleanup is:
- separate visibility metadata production from `projectZ()`
- move to coarse world-space visibility for terrain tiles/cells

### Suggested commit breakdown

1. one atomic terrain cull contract change in `quad.cpp`
2. validation and documentation update
3. separate terrain visibility metadata production from `projectZ()`
4. world-space visibility producer for terrain

### Risk

High.

This is the phase most likely to reintroduce:
- terrain disappearance
- near-camera giant triangles
- silent cull errors

### Explicit deferral

Do not attempt full terrain/world projection unification, water migration, and
terrain visibility producer replacement in one pass.

## Phase 4: Startup-Built Static Terrain Shadow Map

### Objective

Replace camera-driven terrain shadow accumulation with a startup-built,
world-fixed terrain shadow atlas.

### Why this phase matters most for shadow stability

This is the point where "static terrain shadow" becomes architecturally true
instead of just conceptually true.

### Deliverables

- full terrain shadow build at mission load or terrain load completion
- no camera-motion-triggered terrain shadow cache growth during gameplay
- explicit invalidation rules

### Primary files

- [mclib/txmmgr.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/txmmgr.cpp)
- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)
- mission / terrain init path as needed

### Safe first slice

Do not start by raising resolution.

First:
- move static terrain shadow generation timing out of gameplay camera-motion
  logic
- preserve the same shadow content quality initially

That isolates stability changes from quality changes.

### Suggested commit breakdown

1. introduce explicit static terrain shadow build entry point
2. call it at mission/terrain startup
3. disable camera-driven accumulation path
4. document invalidation rules

### Validation

Check:
- no terrain shadow rebuild hitch during normal camera movement
- terrain shadows are present immediately after mission load
- terrain shadow coverage no longer depends on camera travel

### Risk

Medium.

Primary risk:
- mission-start load spike or misordered initialization

## Phase 5: Static Shadow Quality Pass

### Objective

Once static terrain shadow behavior is stable, spend budget on resolution,
bias, and filtering.

### Deliverables

- higher terrain shadow resolution
- reduced sampling banding
- tuned bias and offset behavior
- cleaner large-scale terrain grounding

### Primary files

- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- [shaders/include/shadow.hglsl](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/include/shadow.hglsl)
- terrain fragment shading and shadow uniforms as needed

### Tuning order

1. raise static map resolution
2. tune bias and polygon offset
3. tune PCF / Poisson filtering
4. tune softness policy

### Important rule

Do not mix startup-build architecture work with aggressive filter tuning in the
same initial shadow-stability commit.

### Validation

Check:
- no new acne
- no detached floating shadows
- reduced visible banding at common zoom levels
- unchanged gameplay-time stability

### Risk

Low to medium.

## Phase 6: Dynamic Shadow Reconciliation

### Objective

Re-tune the dynamic local shadow map to complement the now-stable terrain atlas
rather than compensate for it.

### Deliverables

- dynamic shadows read as local detail, not a second global solution
- less disagreement between static and dynamic shadow layers
- stable focus-region behavior

### Primary files

- [mclib/txmmgr.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/txmmgr.cpp)
- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- dynamic shadow object draw path in [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)

### Work items

- retune dynamic focus region and bias against the stable terrain map
- ensure moving object shadows layer cleanly on the terrain baseline
- verify object casters do not needlessly compensate for static map weaknesses

### Validation

Check:
- moving mechs/buildings cast crisp local shadows
- no obvious disagreement between local dynamic shadows and static terrain
  shadows
- no large dynamic frustum popping

### Risk

Medium.

## Phase 7: Post-Shadow Bridge Reduction

### Objective

Shrink the role of the screen-space/post-process shadow pass as more paths own
their shadows directly.

### Deliverables

- clear list of remaining paths that still require the bridge pass
- post pass disabled or bypassed for paths that no longer need it
- simpler final mental model

### Primary files

- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- associated world-space or forward-shadow shader paths

### Suggested approach

Do not delete the post pass wholesale.

Instead:
1. inventory its remaining consumers
2. remove one family at a time as those paths gain proper ownership
3. keep it available as fallback until the inventory is genuinely small

### Risk

Medium.

## Cross-Cutting Validation Strategy

Every phase should be validated against the same set of visual failure modes:

- terrain disappears or goes transparent
- near-camera giant triangles
- broken water
- mission markers or UI taking world-space treatment accidentally
- black or transparent overlays
- camera-motion shadow hitch
- camera-rotation shadow swimming
- terrain and dynamic shadow disagreement

## Priority Queue For Actual Coding

If implementation time is limited, do the phases in this exact priority order:

1. typed terrain overlay / decal path
2. startup-built static terrain shadow map
3. terrain cull / visibility contract cleanup
4. static shadow quality pass
5. dynamic shadow reconciliation
6. post-shadow bridge reduction

Rationale:
- overlay/decal batching is the cleanest architectural cleanup with low-to-medium
  risk
- startup-built static terrain shadows are the biggest shadow stability win
- terrain cull cleanup is essential but highest-risk, so better after the
  surrounding architecture is cleaner

## Explicit Deferrals

These items should not block the roadmap above:

- full water redesign
- full TG_Shape / object GPU migration
- broad particle pipeline modernization
- total deletion of the post shadow bridge before its consumers are mapped
- removing every `projectZ()` call in the engine

## Success Criteria

This roadmap is successful when:

- terrain, overlays, decals, water, and UI have explicit documented contracts
- terrain-adjacent world geometry no longer depends on generic overlay bridge
  behavior
- static terrain shadows are built at startup and remain visually stable during
  play
- dynamic shadows act as local detail rather than a second global system
- the post-process shadow pass is no longer architecturally central

## Suggested Next Session Prompts

### Prompt A: Typed overlay path

"Implement the first safe slice of the typed world-space terrain overlay path:
add the batch plumbing and route alpha cement perimeter overlays only. Do not
touch water, HUD markers, or crater/footprint paths yet. Preserve working
terrain, working water, and the underlayer fix."

### Prompt B: Static terrain shadow startup build

"Refactor static terrain shadows so the terrain shadow atlas is built at startup
instead of accumulating during gameplay camera movement. Do not change dynamic
shadows yet. Preserve current visual output as much as possible before any
resolution or filter tuning."

### Prompt C: Terrain cull contract cleanup

"Audit and fix the terrain cull/submission mismatch in `quad.cpp` as one atomic
terrain slice. Do not broaden the task into water or full `projectZ()` removal.
Define the validation cases first, then change only what is necessary for a
contract-consistent terrain path."
