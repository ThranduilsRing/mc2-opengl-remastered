# Render contract changelog - 2026-05-15

Synthesizer run by mc2-render-contract-synthesizer.

Trigger: Step 10 (final closeout) of the VertexProjectLoop (VPL)
retirement, plan
`docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`.
This is a milestone-driven refresh (the VPL retirement shifted the
contract's ground truth), not an accumulated-notes synthesis.

## Sources consumed

- `docs/superpowers/plans/2026-05-14-vertex-project-loop-retirement.md`
  (v3 -> v3.5 amendment trail; Step 10 section governs this refresh)
- `docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-adversarial-review.md`
  (CRIT-1 catastrophic-axis cull-write placement invariant)
- `docs/superpowers/reviews/2026-05-15-overlay-pz-precursor-adversarial-review.md`
- `docs/superpowers/reviews/2026-05-15-overlay-pz-scoped-redesign-rereview.md`
  (Finding 1, Finding 5 - probe placement)
- `docs/superpowers/reviews/2026-05-15-overlay-pz-v2-bit-identity-proof.md`
  (overlay-pz cv->pz-independent gate, bit-identical-by-construction)
- `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
- Current code at HEAD `96642cc` (every file:line re-grepped this run)

No `docs/observations/*render*.md` notes corpus existed for this run;
this is a plan-milestone-driven refresh. Audit baseline
`docs/render-contract-audit-2026-05-14.md`: not present at HEAD (not
consumed). MEMORY.md (current snapshot) + worktree CLAUDE.md "Render
contract" + "Pending durable artifacts" sections referenced for
discipline (no-emoji, grep-before-cite).

## Changes applied

### ADDED

- (none as standalone new sections; the post-retirement contract fit
  inside the existing Bucket A1 / D1 / Priority 1 structure)

### UPDATED

- Bucket A1 (Terrain base) status block: "still has CPU visibility debt"
  -> "clean (CPU projected-depth debt RETIRED); one decoupled slim CPU
  pass remains by design".
- Bucket A1 Projection owner: `terrainMVP` plus viewport chain -> GPU via
  `clipPos[4]`/Fix-B (thin VS has no terrainMVP uniform); Fix A demoted
  behind `MC2_RING_TRACE` (inert).
- Bucket A1 "Current debt" block -> replaced with "CPU cull + reduction
  producer (by design - NOT debt)": the slim reduction loop
  (`terrain.cpp:1466`) as sole producer of the cull cascade
  (`:1553-1559` BEFORE the `:1564` gate) and the reductions
  (`:1567-1587` -> `:1678 setInverseProject`); the catastrophic-axis
  before-the-continue placement invariant (loose 768u/384u onScreen vs
  strict inView).
- Bucket A1 "Required end state" -> "End state (REACHED 2026-05-15)".
- Bucket D1 (terrain world-space gated by projected depth): status
  active/highest-priority-violation -> RESOLVED 2026-05-15; "Problem" ->
  "Original problem (now closed)"; added "How it was resolved" (VPL body
  deleted; GPU clipPos authority; M2d overlay-pz cv->pz-independent
  on-site re-projection at `quad.cpp:2159-2189`).
- Priority 1: open priority -> "DELIVERED 2026-05-15 (VPL retirement)"
  with the exact post-retirement cull contract + the retired/demoted
  scaffolding family (env gates) + the legacy-lighting both-env
  hard-retirement.

### FLAGGED (TODO USER RESOLVE)

- (none - no contradictions between the plan's claimed end-state and the
  code were found; see "Plan-vs-code consistency" below)

### REMOVED

- (none - per protocol, no section deleted; D1 retained as a
  resolved-bridge record)

### CLARIFIED

- Bucket A3 (terrain overlays/decals target state): added a boundary
  note that the VPL retirement re-homed only the overlay-pz VISIBILITY
  GATE off cv->pz; the dedicated typed world-space batch path is still a
  deferred sibling slice (stub
  `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`).

## Skipped notes

- No notes corpus existed; nothing skipped on staleness grounds. Plan
  prose superseded by the v3.5 amendment trail (e.g. the phantom "5B
  slim pass" cull producer) was NOT transcribed into the contract - the
  contract documents the CODE (the slim loop at `terrain.cpp:1466`), not
  the superseded plan framing.

## Verification trace

All grep-verified at HEAD `96642cc` (2026-05-15):

- `terrain.cpp:1466` - `ZoneScopedN("Terrain::geometry slimReduce")`
  (sole surviving terrain projection loop; VPL Tracy zone GONE)
- `terrain.cpp:1544` - `eye->projectForTerrainAdmission(vertex3D,sp)`
  (re-homed projection)
- `terrain.cpp:1546` - `clipR = (eye->usePerspective &&
  Environment.Renderer != 3) ? onScreenR : inViewR` (identical to deleted
  VPL clipInfo formula)
- `terrain.cpp:1553-1559` - `rv->clipInfo = clipR;` +
  `setObjBlockActive`/`setObjVertexActive` cull cascade write
- `terrain.cpp:1564` - `if (!clipR || !inViewR) continue;` reduction gate
  (cull write is BEFORE this - CRIT-1 invariant)
- `terrain.cpp:1567-1587` - leastZ/mostZ/leastW/mostW/leastWY/mostWY
  reductions
- `terrain.cpp:1671-1675` - yzRange/ywRange derivation
- `terrain.cpp:1678` - `eye->setInverseProject(mostZ,leastW,yzRange,ywRange)`
- `terrain.cpp:1445-1464` - MC2_VPL_CULL/MC2_VPL_REDUCE one-shot
  `event=retired` lines
- zero `vertexProjectLoop`/`VPParitySnap`/`s_vpFast`/`s_vpParity` in
  `mclib/`/`code/`/`GameOS/`/`shaders/` (only historical comments in
  camera.cpp:567, camera.h:661, gos_terrain_indirect.cpp:1691,
  gpu_driven_terrain_solid.comp:195)
- `quad.cpp:2159-2189` - overlay-pz production gate: `vertices[c]->clipInfo
  == 0` sentinel (`:2172`) + `eye->projectForTerrainAdmission(ov3D,osp)`
  (`:2176`); no `vertices[c]->pz` read in the production path
- `quad.cpp:2213` - the ONLY `vertices[c]->pz` read, inside the demoted
  MC2_M2D_PZ_PARITY probe-only local
- `quad.cpp:1348` - `s_lightingGpuAuth = true`;
  `quad.cpp:1364` - `if (!s_lightingGpuAuth ...)` unreachable block
- `mapdata.cpp:1154` - `currentVertex->hazeFactor = 0.0f;` defensive
  zero-init
- `gos_terrain_indirect.cpp:1473` - `g_envRingTrace` (MC2_RING_TRACE) gate
- `gos_terrain_indirect.cpp:1612` - MC2_TERRAIN_INDIRECT_CPU_FALLBACK
- `gos_terrain_indirect.cpp:2121` - MC2_BUCKET_HEADER_TRACE
- `gos_terrain_thin.vert` - Fix-B clipPos[4] sole projection authority,
  terrainMVP uniform REMOVED comment
- `gameos_graphics.cpp:2683-2724` - `[RING_MVP_DELTA v1]` FNV residual,
  per-frame UNGATED (deferred item 4)
- `gos_terrain_lighting.h:38`/`:46` + `terrain_lighting_shared.hglsl:10`
  - dead hazeFactor field + static_assert(==32) (deferred item 1)
- `gos_terrain_lighting.cpp:593` - `vi.hazeFactor = 0.0f;` populate
  neutralize (NOTE: plan cites `:553`, drifted to `:593` - see drift)

## Known limits of this run

- The contract documents CODE at HEAD `96642cc`, not plan aspiration.
  The plan's v3/v3.1/v3.2 prose referenced a phantom "Step 5 slim pass /
  5B" cull producer that never existed in code (corrected by the v3.5
  CRIT-0 amendment). The contract correctly attributes the cull producer
  to the slim reduction loop at `terrain.cpp:1466`, matching the code.
- File:line drift from plan citations: the plan's
  `gos_terrain_lighting.cpp:553` populate-write anchor has drifted to
  `:593` at HEAD; the plan's `terrain.cpp:1601/:1738` cv->pz write
  anchors are stale (the writes are deleted). The contract uses only
  re-grepped HEAD line numbers.
- Buckets A2, B1, B2, C1, D2, D3, Allowed Legacy Containment, Migration
  Policy, Non-Goals, Review Checklist: not touched - the VPL retirement
  did not change their ground truth. D2 (IS_OVERLAY/rhw bridge) is
  related but its end-state is the A3 sibling slice, already
  cross-referenced via the A3 CLARIFY.
- GBuffer1 check script (`scripts/check-render-contract-gbuffer1.sh`):
  not present at HEAD; no GBuffer1 mask semantics were touched by this
  refresh, so no script impact.
- Tracy CPU-recovery quantification: the VPL Tracy zone is confirmed
  GONE, so the plan's `~475 us` figure remains an unmeasured pre-estimate
  (deferred item 5). The contract does not assert a measured perf number.
