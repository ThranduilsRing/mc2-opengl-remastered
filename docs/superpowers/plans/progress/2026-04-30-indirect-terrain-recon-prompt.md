# Recon Session Prompt — Indirect Terrain Draw (Stage 0)

> **What this file is:** the self-contained prompt for the recon session
> that resolves the 9 open questions in the indirect-terrain spec before
> any code lands. Paste the body below into a fresh Claude Code session
> (or schedule it via `/schedule`). The recon session outputs its findings
> to a sibling file:
> `docs/superpowers/plans/progress/<YYYY-MM-DD>-indirect-terrain-recon-handoff.md`

---

## Prompt body (paste verbatim)

You are starting a **recon session** for the FINAL slice of MC2's CPU→GPU
offload arc: **indirect terrain draw**. The brainstorm and spec are signed
off. Code is gated until 9 specific open questions are resolved. Your job
is to investigate each, document findings, and hand off to the
implementation-plan session.

This session is **read-only research**. Do NOT write code. Do NOT redesign
the spec. Do NOT expand scope. Do NOT start the implementation plan. Just
answer the 9 questions, with citations, and write a handoff doc.

### READ FIRST (in order, NON-SKIPPABLE)

The spec inherits all the "why" from the brainstorm; the brainstorm
inherits the "what" from the orchestrator. Read in this order:

1. **Spec (your scope-of-work):**
   `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`
   — The "Recon items" section lists 8 questions. The "Constraints"
   section gotcha #9 raised a 9th question (depth-fudge parity).

2. **Brainstorm (decisions you must NOT relitigate):**
   `docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md`
   — Q1=(b), Q2=(a), Q3=extend M2 recipe to dense, Q4=per-thin-record
   byte-compare, Q5=retire main emit only, Q6=chokepoint pattern,
   Q7=4-gate ladder. These are settled. Recon answers HOW to implement,
   not WHETHER.

3. **Orchestrator (workstream context):**
   `docs/superpowers/cpu-to-gpu-offload-orchestrator.md`
   — Status board, scope rules (stock missions only), adjacent systems.

4. **Predecessor pattern (reusable template):**
   `memory/water_ssbo_pattern.md`
   — The renderWater Stage 2+3 architectural shape that this slice
   mirrors at terrain scale.

5. **GPU-direct gotchas (the 9 traps every fast path hits):**
   `memory/gpu_direct_renderer_bringup_checklist.md`

Skim only what's needed for each recon item; don't try to memorize
everything before starting.

### THE 9 RECON ITEMS

Each item has: (a) the question, (b) entry points (file:line citations
from the spec — verify against current code, they may have drifted), (c)
what "answered" looks like.

#### Item 1 — Render-order placement

**Question:** Where exactly does the new indirect-terrain draw hook in
`code/gamecam.cpp`? Specifically: does it replace the existing
`mcTextureManager->renderLists()` call, sit between two phases of it, or
run before/after as a separate pass?

**Entry points:**
- `code/gamecam.cpp:243` — current `renderLists()` call (legacy buckets flush here).
- `code/gamecam.cpp:254` — `land->renderWaterFastPath()` post-renderLists hook (precedent pattern).
- `mclib/txmmgr.cpp` — `renderLists()` body. Inventory which buckets it drains in what order.

**Read:** `memory/render_order_post_renderlists_hook.md`.

**Answered when you can name:** the exact call-site location and which
texture-manager bucket(s) hold terrain main-emit (vs decals, objects,
shadow-casters). Also: confirm what depth/blend state is active at that
point in the frame so the bridge can save/restore correctly.

#### Item 2 — Thin-record format coverage

**Question:** Does the existing 32 B M2 thin record carry ALL per-frame
state currently computed in `setupTextures`? If gaps exist, list them.

**Entry points:**
- `mclib/quad.cpp:429` — `TerrainQuad::setupTextures` body start.
- `mclib/quad.cpp:432` — first Tracy zone inside (`quadSetupTextures admission / early guards`).
- `mclib/terrain.cpp:1681` — outer Tracy zone wrapping the per-frame loop.
- `gos_terrain_patch_stream.cpp` — current 32 B `TerrainQuadThinRecord` definition.

**Audit candidates:** overlay UV state if overlay handle is mutable
per-frame; alpha modulation factors; per-tri pzValid bits; mineState bits
(slice 2b cached on recipe — confirm still applies); detail UV
animation; selection highlight bits.

**Answered when you can list:** every per-frame computation in
`setupTextures`'s body, classified as (i) already in thin record, (ii)
needs to be added to thin record, (iii) belongs in recipe + invalidated
on mutation event, (iv) belongs in a uniform.

#### Item 3 — Bridge destruction path

**Question:** Does bridge destruction route through an existing chokepoint
(`setTerrain` / `setOverlay`) or have its own logic? Same question for
crater / damage-to-terrain-type events.

**Entry points:**
- `mclib/terrain.cpp:875` — `Terrain::setTerrain` (routes to mapData->setTerrain).
- `mclib/mapdata.cpp:1293` — `MapData::setTerrain` body.
- `mclib/mapdata.cpp:213` — `MapData::invalidateTerrainFaceCache`.
- Existing chokepoint callers: `mclib/mapdata.cpp:149`, `:191`, `:1359`.

**Search terms (Grep):** `setOverlay`, `bridgeDestroy`, `Bridge::destroy`,
`pBridge`, `craterTexture`, `scorchType`, `MC_GRASS_TYPE` mutation.

**Answered when you have:** a table of every in-game terrain-mutation
callsite, classified as (i) already routes through `setTerrain` /
`invalidateTerrainFaceCache`, (ii) needs a new `invalidateRecipeFor(vertexNum)`
hook, (iii) rare enough to rebuild the whole SSBO, (iv) editor-only
(out of scope).

#### Item 4 — Stock smoke missions: in-game terrain mutations

**Question:** Do mc2_01, mc2_03, mc2_10, mc2_17, mc2_24 trigger ANY
in-game terrain mutation? If zero, recipe is post-load immutable on the
smoke set and Q6's complexity collapses.

**Method:** static analysis of mission files + `[TERRAIN_INDIRECT v1]
event=invalidate` log capture during a tier1 smoke run (once Stage 1 lands
defensive hooks). For recon, you can only do the static-analysis half.

**Mission file locations:** under `data/missions/` or wherever the FST
binds them. Mines are confirmed for mc2_24 (per slice 2b memory). Bridges
may exist on mc2_10 or mc2_17 (verify).

**Answered when you can say:** "On stock smoke set, in-game terrain
mutations are: NONE / [list of events with mission + frequency]."

#### Item 5 — `drawMine` on stock smoke missions

**Question:** Does the legacy `drawMine` path still draw clean over the
new indirect terrain when `MC2_TERRAIN_INDIRECT=1`? Mainly relevant on
mc2_24.

**Entry points:**
- `mclib/quad.cpp:4136` — `TerrainQuad::drawMine`.
- `mclib/terrain.cpp:963` — minePass loop call.
- `mclib/terrain.cpp:929` — comment about 3-pass split.

**Method:** read the drawMine path to identify what state it depends on
(depth state, terrain handles, vertex pointers). Confirm none of that
changes when indirect-terrain takes over the main-emit path.

**Answered when you can say:** "drawMine inherits depth state from X and
reads vertex state from Y; both remain valid under indirect-terrain
because [reason]."

#### Item 6 — Killswitch semantics

**Question:** Confirm `MC2_TERRAIN_INDIRECT=0` is a full legacy fallback
(no recipe build, no thin record, no indirect draw). Define the exact
branch points.

**Entry points:**
- `mclib/terrain.cpp:575` — `Terrain::primeMissionTerrainCache` start.
- `mclib/terrain.cpp:599` — existing `WaterStream::Build()` call (pattern).
- `mclib/terrain.cpp:1681` — `quadSetupTextures` per-frame Tracy zone.
- `code/gamecam.cpp:243` — render-order hook.

**Answered when you can list:** the 3-5 specific branch points where
`MC2_TERRAIN_INDIRECT=0` short-circuits the new path and routes to legacy
unchanged.

#### Item 7 — Tracy aggregator zone

**Question:** Add a `Terrain::TotalCPU` per-frame aggregator (sum of
vertexProjectLoop + ThinRecordPack + drawPass minus the now-skipped
quadSetupTextures iteration), or stick to per-zone deltas?

**Entry points:**
- `memory/tracy_profiler.md` — Tracy zone conventions.
- `gos_profiler.h` — `ZoneScopedN` macro.
- `mclib/terrain.cpp:1681` and other Tracy-instrumented zones in
  Terrain::geometry / Terrain::render.

**Answered when you can say:** "Recommend [aggregator / per-zone
deltas] because [reason]; aggregator implementation would be
[ZoneScopedN block at file:line wrapping calls X/Y/Z]."

#### Item 8 — Two-PR promotion sequence

**Question:** Document the convention for shipping the slice opt-in (PR 1),
soaking, then flipping default (PR 2).

**Entry points:**
- `memory/patchstream_shape_c.md` — Shape C precedent (commit `aee39cc` on
  the flip; killswitch convention).
- `memory/renderwater_fastpath_stage2.md` — renderWater is awaiting
  default-on flip (find out if it's been done since 2026-04-30).
- Orchestrator status board "Update protocol" section.

**Answered when you can write:** the commit-message template for PR 1 and
PR 2, plus the orchestrator status-board update protocol after each.

#### Item 9 — Depth-fudge parity (added during spec review 2026-04-30)

**Question:** Does the M2 thin VS at `shaders/gos_terrain_thin.vert:137`
need `+ 0.001` (TERRAIN_DEPTH_FUDGE) added to match legacy CPU emit, or
is the current `screen.z = clip.z * rhw;` already correct for the thin
path's draw state?

**Background:** Legacy CPU emit (e.g. `mclib/quad.cpp` ~line 1500-2156)
applies `+ TERRAIN_DEPTH_FUDGE` (`0.001f`) to every terrain triangle
z-coord at the per-vertex emit sites. The thin VS does NOT apply it. Two
possibilities:
- (a) The thin path's draw state doesn't need it (e.g., depth-write-off
  or different bucket ordering means ties don't matter).
- (b) Latent z-fighting bug between terrain and props/decals at certain
  zoom levels that nobody has reported because thin path may be a
  newer/less-exercised code path.

**Entry points:**
- `shaders/gos_terrain_thin.vert:130-141` — current double-projection
  block.
- `mclib/quad.cpp` (search for `TERRAIN_DEPTH_FUDGE`) — emit sites.
- `memory/gpu_direct_depth_state_inheritance.md` — fudge rationale.
- `memory/terrain_mvp_gl_false.md` — projection chain.

**Method:** Read both code paths. If the thin path's bucket draws to a
state where depth-write is off, fudge is irrelevant. If depth-write is
on, the absence of fudge would be a latent z-fighting bug that may not
have been reported because props are drawn AFTER terrain anyway and
draw-order resolves ties.

**Answered when you can say:** "Thin VS [needs / does not need] the
fudge because [rationale]. Implementation impact for indirect-terrain
draw: [add `+ 0.001` in VS / leave VS alone / add a new VS variant]."

### CONSTRAINTS (load-bearing, must respect)

- **Read-only investigation.** No code edits. No spec edits. No commits.
  Output is one handoff doc.
- **Stay in scope.** Do NOT redesign Q1-Q7 from the brainstorm. Do NOT
  expand scope to drawPass elimination (Q1 (c)) or drawMine retirement
  (Q5 follow-up). Do NOT validate against mod content.
- **Don't relitigate gotchas.** The 9 GPU-direct gotchas are confirmed
  load-bearing. The 3 "NOT applicable to terrain" memories
  (`cull_gates_are_load_bearing`, `tgl_pool_exhaustion_is_silent`,
  `shadow_caster_eligibility_gate`) are confirmed scoped-out. Don't
  re-derive these distinctions.
- **Don't write the implementation plan.** That's the next session's
  job, after recon lands.
- **CLAUDE.md rules apply.** Stock install must remain playable.
  Project conventions on memory/CLAUDE.md discipline. Smoke gate
  default uses `--duration 20` not 120.

### TOOLS

Primary: Read, Grep, Glob.
Secondary: Bash (for `git log`, `gh issue view`, etc.).
Avoid: Write (except the final handoff doc), Edit (none), Agent
(unnecessary for read-only research).

### DELIVERABLE

A handoff doc at:
`docs/superpowers/plans/progress/<TODAY_YYYY-MM-DD>-indirect-terrain-recon-handoff.md`

Use today's date in the filename. Structure:

```markdown
# Indirect Terrain Draw — Recon Handoff

**Date:** YYYY-MM-DD
**Predecessor:** docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md
**Successor:** [implementation-plan session, when triggered]

## TL;DR
[1-2 sentence summary: which recon items resolved cleanly, which surfaced
new constraints, anything that should change the spec.]

## Item-by-item findings

### Item 1 — Render-order placement
**Status:** Resolved / Partially-resolved / Open with [reason].
**Finding:** [what the answer is, with citations]
**Implementation impact:** [what the implementation plan should encode]

### Item 2 — Thin-record format coverage
[same shape]

... (Items 3-9)

## New findings / spec amendments

[If recon surfaces something the spec didn't anticipate — e.g., a new
mutation event, a missing per-frame state field — note it here. The
implementation-plan session will absorb these into the plan; if a
finding is large enough to require a spec amendment, flag it for user
review.]

## Open follow-ups (not blocking implementation)

[Things you noticed but that are outside the 9 items.]

## Recon time / token budget used

[Brief log: which files read, which greps run, approx duration. Helps
calibrate future recon estimates.]
```

Length target: ~150-300 lines. Tight findings beat thorough prose.

### START BY

1. Read the spec (file #1 above) end-to-end. ~10 minutes.
2. Skim the brainstorm (#2) for any decision context you need.
3. Open a TodoWrite with one task per recon item.
4. Work item-by-item. Each item should take 10-30 minutes of
   investigation. If one balloons past 1 hour, flag it as
   "needs-spec-amendment" and move to the next.
5. Synthesize the handoff doc at the end.

Good luck. The implementation-plan session inherits everything you
write down — be specific.

---

## Reviewer notes (for the user, not the recon session)

This prompt is self-contained: a fresh session can pick it up cold.
Cited entry points were verified against current code on 2026-04-30
during spec drafting; if recon finds drift, that's a finding to record.

Three escalation paths the recon session may hit:

1. **Item finds something the spec mis-stated.** Flag in the handoff
   doc's "New findings / spec amendments" section. The user reviews
   before the implementation plan opens.

2. **Item ballons past 1 hour.** That's a sign the question was
   misframed in the spec. Move on; flag at the top of the handoff;
   user decides whether to defer to the implementation plan or open
   a separate investigation session.

3. **Item surfaces a load-bearing constraint that changes Q1-Q7
   decisions.** Stop and surface to user. Don't redesign solo.

Recon is the cheapest stage — investing time here prevents the
implementation plan from chasing unverified assumptions. But it's also
the easiest stage to scope-creep into. The "9 items, 1 handoff doc"
shape is deliberately tight.
